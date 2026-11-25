#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>

// One image-assembly and bootloader exchange for an ATtiny1616 Slave Module.
//
// The wire protocol is defined by the slave repository's PROTOCOL.md,
// "OTA bootloader":
//
//   [0xAA][addr][cmd][len][payload ... ][crc16 hi][crc16 lo]
//
// Every byte after the 0xAA sync marker is escape-encoded (0x5C -> 5C 00,
// 'm' -> 5C 01) so a raw 'm' never appears inside OTA traffic: bystander
// modules only start a command line at 'm' and can therefore never be
// affected by firmware bytes streaming past them. CRC-16/CCITT-FALSE
// (poly 0x1021, init 0xFFFF, no reflection, no final XOR) covers
// addr/cmd/len/payload and is sent big-endian; the application image is
// verified with CRC-32 IEEE; all payload fields are big-endian.
//
// The image is assembled from MQTT messages on <prefix>/command/firmware:
//
//   {"action":"begin","module":N}    start collecting an image for module N
//   {"action":"chunk","data":"..."} append base64 image bytes
//   {"action":"flash"}               run the flash session
//   {"action":"cancel"}              discard the image, or abort the session
//   {"action":"reset","module":N}    boot a module stuck in the bootloader
//
// This module owns the guards, boot policy, retries, status and post-flash
// display recovery. Callers only deliver a command and supply pump, notifier
// and recovery hooks; bus and MQTT traffic travels through thin adapters.
class ModuleFirmwareSession {
  public:
    using Hook = void (*)();
    using RecoveryHook = void (*)(uint8_t moduleId);

    // Interpret one firmware command. `scanActive` lets the session refuse a
    // flash while a bus scan owns the bus. Returns true when the caller
    // should publish a Status Payload.
    bool handleAction(JsonObject command, bool scanActive);

    // ---- image assembly ----
    bool beginImage(uint8_t moduleId);
    bool appendChunk(const char* base64);
    bool imageReady() const { return collecting_ && size_ > 0; }
    bool collecting() const { return collecting_; }
    uint16_t imageSize() const { return size_; }

    // ---- session ----
    // Blocking; true when the module verified and rebooted into the new
    // application. The collected image is kept on failure so "flash" can be
    // retried without re-sending chunks.
    bool runSession();
    void cancel();
    bool resetModule(uint8_t moduleId);
    bool active() const { return active_; }

    // ---- status reporting ----
    const char* stateString() const;
    uint8_t targetModule() const { return moduleId_; }
    // OTA protocol generation answered by the current/last bootloader.
    uint8_t bootloaderVersion() const { return bootloaderVersion_; }
    // 0..100 while a session runs, 100 when done, -1 otherwise.
    int progress() const;
    const char* error() const { return error_.c_str(); }

    // Pump keeps MQTT alive during the blocking session. Notifier publishes
    // progress. Recovery restores the module's requested display character
    // after a successful flash.
    void setPump(Hook pump) { pump_ = pump; }
    void setNotifier(Hook notifier) { notifier_ = notifier; }
    void setRecovery(RecoveryHook recovery) { recovery_ = recovery; }

    static constexpr uint16_t PAGE_SIZE = 64;
    static constexpr uint16_t APP_MAX_SIZE = 14336;
    static constexpr uint16_t MAX_PAGES = APP_MAX_SIZE / PAGE_SIZE;
    static constexpr uint8_t MAX_RETRIES = 3;

  private:
    enum class State : uint8_t {
        Idle, Collecting, Entering, Flashing, Verifying, Done, Cancelled, Failed
    };

    struct Response {
        uint8_t cmd = 0;
        uint8_t len = 0;
        uint8_t payload[3] = {};
    };

    bool enterBootloader();
    bool waitForBootloader(bool appAcked);
    String readAsciiLine(unsigned long timeoutMs);
    bool startUpdate();
    bool writePages();
    bool finishUpdate();
    void cancelSession(bool pagesAttempted);
    void setState(State state) { state_ = state; }
    void fail(const String& message);
    void failNack(const char* what, uint8_t code);
    void sendFrame(uint8_t cmd, const uint8_t* payload, uint8_t len);
    bool recvResponse(unsigned long timeoutMs, Response& out);
    bool exchange(uint8_t cmd, const uint8_t* payload, uint8_t len, Response& out);
    void pump() { if (pump_) pump_(); }
    void notify() { if (notifier_) notifier_(); }
    void recover() { if (recovery_) recovery_(moduleId_); }
    void pumpDelay(unsigned long ms);

    State state_ = State::Idle;
    bool active_ = false;
    bool abort_ = false;
    bool collecting_ = false;
    // A page may have landed even when its ACK was lost. Past this point a
    // cancelled session must leave the module in the bootloader.
    bool pageAttempted_ = false;
    uint32_t b64acc_ = 0;
    uint8_t b64bits_ = 0;
    uint8_t moduleId_ = 0;
    uint8_t bootloaderVersion_ = 0;
    uint16_t size_ = 0;
    uint16_t pagesDone_ = 0;
    uint16_t totalPages_ = 0;
    uint32_t imageCrc_ = 0;
    String error_;
    uint8_t image_[APP_MAX_SIZE];

    Hook pump_ = nullptr;
    Hook notifier_ = nullptr;
    RecoveryHook recovery_ = nullptr;
};

extern ModuleFirmwareSession firmwareSession;
