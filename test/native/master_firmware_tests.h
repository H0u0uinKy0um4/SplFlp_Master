#pragma once

#include <unity.h>

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstring>
#include <string>
#include <vector>

#include "BusTrace.h"
#include "ContractSnapshot.h"
#include "Version.h"
#include "ota/FirmwareUpdater.h"
#include "ota/MasterFirmwareSession.h"
#include "ota/Sha256.h"

namespace {

int rebootCalls = 0;

void countReboot() { rebootCalls++; }

class FakeUpdater final : public FirmwareUpdater {
  public:
    uint32_t capacity() const override { return capacity_; }
    bool begin(uint32_t size) override {
        beginCalls++;
        expectedSize_ = size;
        written_.clear();
        error_ = "";
        return beginOk_;
    }
    bool setExpectedDigest(const char* sha256Hex) override {
        digest_ = sha256Hex;
        return true;
    }
    size_t write(const uint8_t* data, size_t len) override {
        if (!writeOk_) return 0;
        written_.insert(written_.end(), data, data + len);
        return len;
    }
    bool finish() override {
        finishCalls++;
        if (!finishOk_) {
            error_ = "digest mismatch";
            return false;
        }
        return true;
    }
    void abort() override { abortCalls++; }
    uint32_t written() const override {
        return static_cast<uint32_t>(written_.size());
    }
    const char* error() const override { return error_.c_str(); }

    uint32_t capacity_ = 4096;
    bool beginOk_ = true;
    bool writeOk_ = true;
    bool finishOk_ = true;
    uint32_t expectedSize_ = 0;
    int beginCalls = 0;
    int finishCalls = 0;
    int abortCalls = 0;
    std::string digest_;
    std::string error_;
    std::vector<uint8_t> written_;
};

std::string sha256Hex(const std::vector<uint8_t>& data) {
    sf::Sha256 hash;
    if (!data.empty()) hash.update(data.data(), data.size());
    char hex[65];
    hash.hex(hex);
    return std::string(hex);
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t word = (uint32_t)data[i] << 16 |
                              (i + 1 < data.size() ? (uint32_t)data[i + 1] << 8 : 0) |
                              (i + 2 < data.size() ? (uint32_t)data[i + 2] : 0);
        out += alphabet[(word >> 18) & 0x3F];
        out += alphabet[(word >> 12) & 0x3F];
        out += i + 1 < data.size() ? alphabet[(word >> 6) & 0x3F] : '=';
        out += i + 2 < data.size() ? alphabet[word & 0x3F] : '=';
    }
    return out;
}

std::vector<uint8_t> sampleImage(size_t len) {
    std::vector<uint8_t> data;
    for (size_t i = 0; i < len; i++) data.push_back((uint8_t)(i * 7 + 3));
    return data;
}

void configured(MasterFirmwareSession& session, FakeUpdater& updater) {
    session.setUpdater(&updater);
    session.setPump(nullptr);
    session.setNotifier(nullptr);
    session.setReboot(countReboot);
    session.setLogger(nullptr);
}

}  // namespace

inline void test_command_trace_suppresses_large_base64_payload() {
    const String small =
        formatCommandTrace("splitflap/command/display", "{\"text\":\"17\"}");
    TEST_ASSERT_EQUAL_STRING("<- splitflap/command/display {\"text\":\"17\"}",
                             small.c_str());

    String large;
    for (int i = 0; i < 1000; i++) large += 'A';
    const String traced =
        formatCommandTrace("splitflap/command/master_firmware", large);
    TEST_ASSERT_NOT_NULL(strstr(traced.c_str(), "[1000 bytes, payload suppressed]"));
    TEST_ASSERT_NULL(strstr(traced.c_str(), "AAAA"));
}

inline void test_master_sha256_known_vector() {
    const std::vector<uint8_t> abc = {'a', 'b', 'c'};
    TEST_ASSERT_EQUAL_STRING(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad",
        sha256Hex(abc).c_str());
}

inline void test_master_begin_rejects_invalid_inputs_before_writing() {
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    session.beginImage("1.2.0.0", "not-a-digest", 16);
    session.beginImage("1.2.0.0", std::string(64, '0').c_str(), 0);
    session.beginImage("1.2.0.0", std::string(64, '0').c_str(), 5000);
    session.beginImage("", std::string(64, '0').c_str(), 16);

    TEST_ASSERT_EQUAL_STRING("idle", session.stateString());
    TEST_ASSERT_EQUAL_UINT(0, updater.beginCalls);
    TEST_ASSERT_EQUAL_UINT(0, updater.written_.size());
    TEST_ASSERT_EQUAL_UINT(0, updater.finishCalls);
}

inline void test_master_streams_verifies_and_applies() {
    rebootCalls = 0;
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    const auto image = sampleImage(300);
    const std::string digest = sha256Hex(image);
    TEST_ASSERT_TRUE(session.beginImage("1.2.0.0", digest.c_str(), image.size()));
    TEST_ASSERT_EQUAL_STRING("collecting", session.stateString());
    TEST_ASSERT_EQUAL_UINT(1, updater.beginCalls);
    TEST_ASSERT_EQUAL_STRING(digest.c_str(), updater.digest_.c_str());

    const std::string encoded = base64Encode(image);
    const std::string first = encoded.substr(0, 100);
    const std::string second = encoded.substr(100);
    TEST_ASSERT_TRUE(session.appendChunk(first.c_str()));
    TEST_ASSERT_TRUE(session.appendChunk(second.c_str()));

    TEST_ASSERT_EQUAL_UINT(image.size(), updater.written_.size());
    TEST_ASSERT_EQUAL_INT(0, memcmp(image.data(), updater.written_.data(), image.size()));
    TEST_ASSERT_EQUAL_INT(100, session.progress());
    TEST_ASSERT_EQUAL_UINT(0, updater.abortCalls);

    TEST_ASSERT_TRUE(session.apply());
    TEST_ASSERT_EQUAL_UINT(1, updater.finishCalls);
    TEST_ASSERT_EQUAL_INT(1, rebootCalls);
    TEST_ASSERT_EQUAL_STRING("rebooting", session.stateString());
}

inline void test_master_bad_digest_never_commits() {
    rebootCalls = 0;
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    const auto image = sampleImage(64);
    TEST_ASSERT_TRUE(session.beginImage("1.2.0.0", std::string(64, '0').c_str(),
                                        image.size()));
    TEST_ASSERT_FALSE(session.appendChunk(base64Encode(image).c_str()));

    TEST_ASSERT_EQUAL_STRING("failed", session.stateString());
    TEST_ASSERT_EQUAL_UINT(1, updater.abortCalls);
    TEST_ASSERT_FALSE(session.apply());
    TEST_ASSERT_EQUAL_UINT(0, updater.finishCalls);
    TEST_ASSERT_EQUAL_INT(0, rebootCalls);
}

inline void test_master_rejects_image_larger_than_declared() {
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    const auto image = sampleImage(5);
    TEST_ASSERT_TRUE(session.beginImage("1.2.0.0", sha256Hex(image).c_str(), 4));
    TEST_ASSERT_FALSE(session.appendChunk(base64Encode(image).c_str()));
    TEST_ASSERT_EQUAL_STRING("failed", session.stateString());
    TEST_ASSERT_EQUAL_UINT(1, updater.abortCalls);
    TEST_ASSERT_EQUAL_UINT(0, updater.finishCalls);
}

inline void test_master_apply_refuses_incomplete_image() {
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    const auto image = sampleImage(40);
    TEST_ASSERT_TRUE(session.beginImage("1.2.0.0", sha256Hex(image).c_str(),
                                        image.size() + 1));
    TEST_ASSERT_TRUE(session.appendChunk(base64Encode(image).c_str()));
    TEST_ASSERT_FALSE(session.apply());

    TEST_ASSERT_EQUAL_STRING("failed", session.stateString());
    TEST_ASSERT_EQUAL_UINT(1, updater.abortCalls);
    TEST_ASSERT_EQUAL_UINT(0, updater.finishCalls);
}

inline void test_master_concurrent_begin_is_rejected() {
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    const auto image = sampleImage(32);
    TEST_ASSERT_TRUE(session.beginImage("1.2.0.0", sha256Hex(image).c_str(),
                                        image.size()));
    TEST_ASSERT_FALSE(session.beginImage("9.9.9.9", sha256Hex(image).c_str(),
                                         image.size()));
    TEST_ASSERT_EQUAL_UINT(1, updater.beginCalls);
    TEST_ASSERT_EQUAL_STRING("collecting", session.stateString());
    TEST_ASSERT_EQUAL_STRING("a master firmware session is already active",
                             session.error());
}

inline void test_master_cancel_discards_collection() {
    MasterFirmwareSession session;
    FakeUpdater updater;
    configured(session, updater);

    const auto image = sampleImage(64);
    TEST_ASSERT_TRUE(session.beginImage("1.2.0.0", sha256Hex(image).c_str(),
                                        image.size()));
    TEST_ASSERT_TRUE(
        session.appendChunk(base64Encode(std::vector<uint8_t>(image.begin(),
                                                             image.begin() + 16)).c_str()));
    session.cancel();

    TEST_ASSERT_EQUAL_STRING("cancelled", session.stateString());
    TEST_ASSERT_EQUAL_UINT(1, updater.abortCalls);
    TEST_ASSERT_EQUAL_UINT(0, updater.finishCalls);
    session.cancel();
    TEST_ASSERT_EQUAL_STRING("idle", session.stateString());
}

inline void test_master_status_payload_is_always_complete() {
    JsonDocument doc;
    MasterFirmwareStatusSnapshot status;
    status.state = "collecting";
    status.progress = 42;
    status.received = 84;
    status.size = 200;
    status.targetVersion = "1.2.0.0";

    applyMasterFirmwareStatus(doc.to<JsonObject>(), status);
    applyMasterIp(doc.as<JsonObject>(), "10.0.1.15");

    TEST_ASSERT_EQUAL_STRING("collecting", doc["master_ota"]["state"]);
    TEST_ASSERT_EQUAL_INT(42, doc["master_ota"]["progress"]);
    TEST_ASSERT_EQUAL_UINT(84, doc["master_ota"]["received"]);
    TEST_ASSERT_EQUAL_UINT(200, doc["master_ota"]["size"]);
    TEST_ASSERT_EQUAL_STRING("1.2.0.0", doc["master_ota"]["version"]);
    TEST_ASSERT_EQUAL_STRING("1.2.0.0", doc["master_ota"]["target_version"]);
    TEST_ASSERT_EQUAL_STRING("", doc["master_ota"]["error"]);
    TEST_ASSERT_EQUAL_STRING("10.0.1.15", doc["ip"]);
}
