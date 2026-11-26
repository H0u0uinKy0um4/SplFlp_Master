#include "ota/MasterFirmwareSession.h"

#include <cstring>

MasterFirmwareSession masterFirmwareSession;

namespace {

constexpr size_t DECODE_BUFFER_SIZE = 512;
constexpr uint32_t PUBLISH_EVERY_BYTES = 65536;

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

void MasterFirmwareSession::handleAction(JsonObject command) {
    const char* action = command["action"] | "";

    if (strcmp(action, "begin") == 0) {
        const long long size = command["size"] | -1;
        beginImage(String(command["version"] | ""),
                   String(command["sha256"] | ""),
                   size > 0 && size <= 0xFFFFFFFFLL ? (uint32_t)size : 0);
        return;
    }

    if (strcmp(action, "chunk") == 0) {
        appendChunk(command["data"] | "");
        return;
    }

    if (strcmp(action, "flash") == 0) {
        apply();
        return;
    }

    if (strcmp(action, "cancel") == 0) {
        cancel();
        return;
    }

    reject(String("unknown master firmware action: ") + action);
}

bool MasterFirmwareSession::parseDigest(const String& text, uint8_t out[32]) {
    if (text.length() != 64) return false;
    for (uint8_t i = 0; i < 32; i++) {
        const int hi = hexValue(text[2 * i]);
        const int lo = hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void MasterFirmwareSession::resetImage() {
    hasher_.reset();
    memset(expectedDigest_, 0, sizeof(expectedDigest_));
    memset(actualDigest_, 0, sizeof(actualDigest_));
    targetVersion_ = "";
    error_ = "";
    expectedSize_ = 0;
    received_ = 0;
    base64Acc_ = 0;
    base64Bits_ = 0;
    digestVerified_ = false;
}

bool MasterFirmwareSession::beginImage(const String& version,
                                       const String& sha256, uint32_t size) {
    if (updater_ == nullptr) {
        reject("no updater configured for master firmware");
        return false;
    }
    if (state_ == State::Collecting || applying()) {
        reject("a master firmware session is already active");
        return false;
    }
    if (version.length() == 0 || version.length() > 32) {
        reject("begin requires a version of 1..32 characters");
        return false;
    }
    uint8_t digest[32];
    if (!parseDigest(sha256, digest)) {
        reject("begin requires sha256 as 64 hex characters");
        return false;
    }
    if (size == 0 || size > updater_->capacity()) {
        reject("image size does not fit the inactive OTA slot");
        return false;
    }

    resetImage();
    memcpy(expectedDigest_, digest, sizeof(digest));

    // Reserve the slot, but write nothing and arm no boot marker until the
    // whole image has been received and its digest verified.
    if (!updater_->begin(size)) {
        fail(String("could not begin master firmware update: ") +
             updater_->error());
        updater_->abort();
        return false;
    }
    if (!updater_->setExpectedDigest(sha256.c_str())) {
        fail(String("updater rejected the expected SHA-256: ") +
             updater_->error());
        updater_->abort();
        return false;
    }

    targetVersion_ = version;
    expectedSize_ = size;
    state_ = State::Collecting;
    log("collecting master image " + version + " (" + String(size) + " bytes)");
    notify();
    return true;
}

bool MasterFirmwareSession::appendChunk(const char* base64) {
    if (state_ != State::Collecting) {
        reject("no master firmware collection in progress");
        return false;
    }

    const uint32_t before = received_;
    uint8_t decoded[DECODE_BUFFER_SIZE];
    size_t count = 0;

    auto writeDecoded = [&](size_t len) {
        if (len == 0) return true;
        const size_t written = updater_->write(decoded, len);
        if (written != len) {
            fail(String("master OTA slot write failed after ") +
                 String(received_) + " bytes");
            updater_->abort();
            return false;
        }
        hasher_.update(decoded, len);
        received_ += (uint32_t)len;
        return true;
    };

    for (const char* p = base64; *p != '\0'; p++) {
        const char c = *p;
        if (c == '=') break;  // padding ends the chunk
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;

        const int value = base64Value(c);
        if (value < 0) {
            fail("master firmware chunk is not valid base64");
            updater_->abort();
            return false;
        }

        base64Acc_ = (base64Acc_ << 6) | (uint32_t)value;
        base64Bits_ += 6;
        if (base64Bits_ >= 8) {
            base64Bits_ -= 8;
            if (received_ + count >= expectedSize_) {
                fail("master firmware image exceeds the declared size");
                updater_->abort();
                return false;
            }
            decoded[count++] = (uint8_t)(base64Acc_ >> base64Bits_);
            if (count == sizeof(decoded)) {
                if (!writeDecoded(count)) return false;
                count = 0;
            }
        }
    }
    if (!writeDecoded(count)) return false;

    if (received_ < expectedSize_) {
        if (received_ / PUBLISH_EVERY_BYTES != before / PUBLISH_EVERY_BYTES) {
            notify();
        }
        return true;
    }

    hasher_.digest(actualDigest_);
    if (memcmp(expectedDigest_, actualDigest_, 32) != 0) {
        fail("SHA-256 mismatch; no boot marker was armed");
        updater_->abort();
        return false;
    }

    digestVerified_ = true;
    log("master image verified, ready to flash");
    notify();
    return true;
}

bool MasterFirmwareSession::apply() {
    if (state_ != State::Collecting) {
        reject("flash requires an active master firmware collection");
        return false;
    }
    if (received_ != expectedSize_) {
        fail("master firmware image is incomplete");
        updater_->abort();
        return false;
    }
    if (!digestVerified_) {
        fail("master firmware digest was not verified");
        updater_->abort();
        return false;
    }

    state_ = State::Applying;
    error_ = "";
    log("applying verified master image " + targetVersion_);
    notify();
    pump();

    // finish() verifies the digest again in the updater and only then marks
    // the new slot bootable. ESP-IDF rollback handles a bad first boot.
    if (!updater_->finish()) {
        fail(String("master firmware commit failed: ") + updater_->error());
        updater_->abort();
        return false;
    }

    state_ = State::Rebooting;
    log("master firmware applied, rebooting");
    notify();  // retained state must be readable before the restart
    pump();
    if (reboot_) reboot_();
    return true;
}

void MasterFirmwareSession::cancel() {
    if (state_ == State::Applying || state_ == State::Rebooting) {
        reject("cannot cancel while master firmware is being applied");
        return;
    }
    if (state_ == State::Collecting) {
        updater_->abort();
        resetImage();
        state_ = State::Cancelled;
        log("master firmware collection cancelled");
        notify();
        return;
    }

    resetImage();
    state_ = State::Idle;
    log("master firmware session reset to idle");
    notify();
}

const char* MasterFirmwareSession::stateString() const {
    switch (state_) {
        case State::Collecting: return "collecting";
        case State::Applying:   return "applying";
        case State::Rebooting:  return "rebooting";
        case State::Done:       return "done";
        case State::Failed:     return "failed";
        case State::Cancelled:  return "cancelled";
        case State::Idle:
        default:                return "idle";
    }
}

int MasterFirmwareSession::progress() const {
    switch (state_) {
        case State::Collecting:
            return expectedSize_ == 0 ? 0
                                      : (int)(received_ * 100UL / expectedSize_);
        case State::Applying:
        case State::Rebooting:
        case State::Done:
            return 100;
        default:
            return 0;
    }
}

void MasterFirmwareSession::fail(const String& message) {
    error_ = message;
    state_ = State::Failed;
    log(message);
    notify();
}

void MasterFirmwareSession::reject(const String& message) {
    error_ = message;
    log(message);
    notify();
}

void MasterFirmwareSession::notify() {
    if (notifier_) notifier_();
}

void MasterFirmwareSession::log(const String& line) {
    if (logger_) logger_(line);
}
