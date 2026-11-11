#pragma once
#include <Arduino.h>
#include <cstdint>

// RS485 OTA flasher for the ATtiny1616 slave modules.
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
// The image is assembled from MQTT messages on <prefix>/command/firmware
// (JSON), then flashed in one blocking RS485 session:
//
//   {"action":"begin","module":N}    start collecting an image for module N
//   {"action":"chunk","data":"..."} append base64 image bytes; the decoded
//                                    data is the raw application image as
//                                    linked at 0x0800 (the slave's
//                                    ATtiny1616_OTA build, firmware.bin
//                                    content, gaps 0xFF)
//   {"action":"flash"}               run the flash session
//   {"action":"cancel"}              discard the image, or abort the session
//   {"action":"reset","module":N}    boot a module stuck in the bootloader
//
// Session behaviour (see PROTOCOL.md for the slave side):
//   - "m<id>b" asks the running application to enter the bootloader; a
//     missing reply is fine (the module may already be in the bootloader
//     after a cancelled session or a power cut mid-flash).
//   - PING must answer with staying=1 before anything is written.
//   - START_UPDATE arms the boot flag on the module, so a power cut
//     mid-session leaves it recoverable in the bootloader.
//   - Every 64-byte page is retried up to MAX_RETRIES times; the
//     bootloader stays silent on CRC errors.
//   - FINISH makes the module verify its flash, clear the boot flag and
//     reset itself into the new application.
//   - A cancelled session with pages already written leaves the module in
//     the bootloader (re-run the flash to recover; UPDI is the last
//     resort). Without pages written the module is reset back into its
//     intact application.
//
// The session blocks the main loop for roughly 20-25 s (224 pages at 9600
// baud). MQTT stays alive through the pump hook (wired to client.loop()),
// which is called between pages and during waits, and progress reaches
// Home Assistant through the notifier hook (wired to publishStatus()).
// While the session is active the callers must not put anything else on
// the bus; WifiMqttHandler guards the display/scan/update/calibrate
// commands accordingly.
class SlaveFlasher {
public:
    typedef void (*Hook)();

    // ---- image assembly (MQTT) ----
    // Refused while a session is running.
    bool beginImage(uint8_t moduleId);
    // Appends decoded bytes; refuses when the image would exceed the
    // application region. A failed append discards the collection. Chunks
    // may split the image anywhere: the base64 bit accumulator carries
    // leftover bits across chunks, so only the sender's final padding
    // ends the stream.
    bool appendChunk(const char* base64);
    bool imageReady() const { return collecting_ && size_ > 0; }
    bool collecting() const { return collecting_; }
    uint16_t imageSize() const { return size_; }

    // ---- session ----
    // Blocking; true when the module verified and rebooted into the new
    // application. Keeps the collected image on failure so "flash" can be
    // retried without re-sending the chunks.
    bool runSession();
    // Aborts an active session (checked between pages) or discards a
    // collected image.
    void cancel();
    // Boots a module that sits in the bootloader with its application
    // intact (e.g. after "begin"+"b" without a flash). Requires that no
    // session runs and no image is being collected.
    bool resetModule(uint8_t moduleId);

    bool active() const { return active_; }

    // ---- status reporting (publishStatus "ota" object) ----
    const char* stateString() const;
    uint8_t targetModule() const { return moduleId_; }
    // OTA protocol generation the module's bootloader answered with during
    // the current/last session; 0 before a bootloader PING succeeded.
    uint8_t bootloaderVersion() const { return bootloaderVersion_; }
    // 0..100 while a session runs, 100 when done, -1 otherwise.
    int progress() const;
    const char* error() const { return error_.c_str(); }

    void setPump(Hook pump) { pump_ = pump; }
    void setNotifier(Hook notifier) { notifier_ = notifier; }

    // Flash layout, mirrored from the slave's bootloader/src/protocol.h.
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
    // One ASCII response line (for the m<id>b / m<id>s probes) through the
    // shared rs485read() machinery, which logs the bytes as a SLAVE console
    // line.
    String readAsciiLine(unsigned long timeoutMs);
    bool startUpdate();
    bool writePages();
    bool finishUpdate();
    // Ends the session as cancelled: resets the module back into its intact
    // application only when no page write was even attempted (an ACK can be
    // lost while the page still landed), otherwise leaves it in the
    // bootloader for a re-flash.
    void cancelSession(bool pagesAttempted);
    void setState(State state) { state_ = state; }
    void fail(const String& message);
    void failNack(const char* what, uint8_t code);
    void sendFrame(uint8_t cmd, const uint8_t* payload, uint8_t len);
    bool recvResponse(unsigned long timeoutMs, Response& out);
    bool exchange(uint8_t cmd, const uint8_t* payload, uint8_t len, Response& out);
    void pump() { if (pump_) pump_(); }
    void notify() { if (notifier_) notifier_(); }
    // delay() that keeps MQTT alive through the pump hook.
    void pumpDelay(unsigned long ms);

    State state_ = State::Idle;
    bool active_ = false;       // a session owns the bus
    bool abort_ = false;       // cancel requested mid-session
    bool collecting_ = false;  // image assembly in progress
    // True once any WRITE_PAGE frame has been placed on the bus. A page can
    // be written on the module without its ACK reaching us, so "attempted"
    // (not "acknowledged") is the boundary after which a cancelled session
    // must leave the module in the bootloader instead of resetting it into
    // a possibly half-written application.
    bool pageAttempted_ = false;
    // Base64 bit accumulator carried across appendChunk() calls.
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
};

extern SlaveFlasher slaveFlasher;
