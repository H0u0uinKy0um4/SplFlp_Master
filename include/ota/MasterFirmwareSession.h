#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>

#include "ota/FirmwareUpdater.h"
#include "ota/Sha256.h"

// One master self-update, received on <prefix>/command/master_firmware:
//
//   {"action":"begin","version":"1.2.0.0","sha256":"<64 hex>","size":N}
//   {"action":"chunk","data":"<base64>"}
//   {"action":"flash"}
//   {"action":"cancel"}
//
// Chunks stream straight into the inactive OTA slot. The 4 MB board has only a
// 128 KB data partition, so a ~1.45 MB staging file cannot fit. SHA-256 is
// checked incrementally and again by the updater before it arms the boot
// marker; a bad image therefore never becomes bootable and the old slot keeps
// running. ArduinoOTA on port 3232 stays available as manual recovery.
//
// This module owns the guards, digest/size validation, slot writes, state and
// status. Callers only deliver commands and supply transport hooks.
class MasterFirmwareSession {
  public:
    enum class State : uint8_t {
        Idle, Collecting, Applying, Rebooting, Done, Failed, Cancelled
    };

    using Hook = void (*)();
    using LogHook = void (*)(const String&);

    // Interpret one command. State changes publish the Status Payload through
    // the notifier hook, so callers do not need a second publish.
    void handleAction(JsonObject command);

    // ---- image assembly ----
    bool beginImage(const String& version, const String& sha256, uint32_t size);
    bool appendChunk(const char* base64);
    bool apply();
    void cancel();

    // ---- status reporting ----
    const char* stateString() const;
    const char* targetVersion() const { return targetVersion_.c_str(); }
    const char* error() const { return error_.c_str(); }
    uint32_t received() const { return received_; }
    uint32_t size() const { return expectedSize_; }
    // 0..100 during collection/apply, 100 when rebooting, 0 otherwise.
    int progress() const;
    // Applying/Rebooting: display, scan, update and calibrate must be refused.
    bool applying() const {
        return state_ == State::Applying || state_ == State::Rebooting;
    }
    bool collecting() const { return state_ == State::Collecting; }

    void setUpdater(FirmwareUpdater* updater) { updater_ = updater; }
    void setPump(Hook pump) { pump_ = pump; }
    void setNotifier(Hook notifier) { notifier_ = notifier; }
    void setReboot(Hook reboot) { reboot_ = reboot; }
    void setLogger(LogHook logger) { logger_ = logger; }

  private:
    void fail(const String& message);
    void reject(const String& message);
    void notify();
    void log(const String& line);
    void pump() { if (pump_) pump_(); }
    void resetImage();
    static bool parseDigest(const String& text, uint8_t out[32]);

    State state_ = State::Idle;
    FirmwareUpdater* updater_ = nullptr;
    sf::Sha256 hasher_;
    uint8_t expectedDigest_[32] = {};
    uint8_t actualDigest_[32] = {};
    String targetVersion_;
    String error_;
    uint32_t expectedSize_ = 0;
    uint32_t received_ = 0;
    uint32_t base64Acc_ = 0;
    uint8_t base64Bits_ = 0;
    bool digestVerified_ = false;

    Hook pump_ = nullptr;
    Hook notifier_ = nullptr;
    Hook reboot_ = nullptr;
    LogHook logger_ = nullptr;
};

extern MasterFirmwareSession masterFirmwareSession;
