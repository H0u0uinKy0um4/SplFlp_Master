#include "ota/SlaveFlasher.h"
#include "RS485.h"
#include "Config.h"
#include "Version.h"
#include "Sources.h"
#include <cstring>

SlaveFlasher slaveFlasher;

// Wire protocol constants, mirrored from the slave repository's
// bootloader/src/protocol.h. The framing, CRCs and boot policy are specified
// there (PROTOCOL.md, "OTA bootloader"); this file implements the master
// side of exactly that protocol.

namespace {

constexpr uint8_t kCmdPing        = 0x01;
constexpr uint8_t kCmdStartUpdate = 0x02;
constexpr uint8_t kCmdWritePage   = 0x03;
constexpr uint8_t kCmdFinish      = 0x04;
constexpr uint8_t kCmdReset       = 0x05;

constexpr uint8_t kRespAck  = 0x81;
constexpr uint8_t kRespNack = 0x82;

constexpr uint8_t kSync    = 0xAA;
constexpr uint8_t kEsc     = 0x5C;
constexpr uint8_t kEscEsc  = 0x00;   // 0x5C 0x00 encodes 0x5C
constexpr uint8_t kEscM    = 0x01;   // 0x5C 0x01 encodes 0x6D
constexpr uint8_t kForbidden = 0x6D;  // 'm': never raw inside a frame

// Bootloader PING payload[0]. A mismatch aborts before START_UPDATE, so an
// incompatible bootloader is never written to.
constexpr uint8_t kOtaProtocolVersion = OTA_PROTOCOL_VERSION;

// Largest response payload (PING info: version, module id, staying).
constexpr uint8_t kRespMaxPayload = 3;
// Decoded response body: addr, cmd, len + payload + crc16.
constexpr uint8_t kRespBodyMax = 3 + kRespMaxPayload + 2;

// Largest request payload (WRITE_PAGE: page number u16 + 64 data bytes).
constexpr uint8_t kReqMaxPayload = 2 + SlaveFlasher::PAGE_SIZE;   // 66

// Worst wire size of a WRITE_PAGE frame: the sync byte plus twice the
// logical body (every body byte may double through escaping). The body is
// addr, cmd, len, the 66-byte payload and the CRC-16 — 71 bytes.
constexpr size_t kWireMax = 1 + 2 * (3 + kReqMaxPayload + 2);   // 143

// --- Session timing -------------------------------------------------------

// Response timeout for one frame exchange. A page write (flash erase/write
// plus the reply) takes ~10 ms on the slave; 250 ms leaves a wide margin.
constexpr unsigned long RESPONSE_TIMEOUT_MS = 250;
// FINISH makes the slave compute a CRC-32 over the whole application region
// (~70 ms at 20 MHz) before it answers.
constexpr unsigned long FINISH_TIMEOUT_MS = 500;
// The application delays 100 ms after acking m<id>b and then resets; the
// bootloader comes up within another ~100 ms.
constexpr unsigned long ENTER_BOOT_DELAY_MS = 400;
// Total budget for reaching the bootloader. A module finishing a blocking
// move (worst case ~5 s) is deaf until it lands, so the entry command must
// be retried well past one move window.
constexpr unsigned long ENTER_TOTAL_TIMEOUT_MS = 10000;
constexpr unsigned long ENTER_RETRY_MS = 250;
// How long the freshly flashed application may take to come up (it may
// auto-home first) before the caller re-applies display state and rescans.
constexpr unsigned long BOOT_SETTLE_MS = 500;
// How long to probe m<id>s after FINISH lost its ACK: a running application
// proves the verify succeeded (only a verified FINISH or RESET clears the
// boot flag), but it may be busy auto-homing right after boot.
constexpr unsigned long APP_PROBE_TIMEOUT_MS = 4000;

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout) —
// identical to ota_crc16_byte() in the bootloader's codec.h.
uint16_t crc16Byte(uint16_t crc, uint8_t data) {
    crc ^= (uint16_t)data << 8;
    for (uint8_t i = 0; i < 8; i++) {
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// CRC-32 IEEE (reflected 0xEDB88320, init/xorout 0xFFFFFFFF) — the value
// Python's binascii.crc32 produces over the application image.
uint32_t crc32Byte(uint32_t crc, uint8_t data) {
    crc ^= data;
    for (uint8_t i = 0; i < 8; i++) {
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    }
    return crc;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// NACK error codes (protocol.h ERR_*), for log/status messages only.
const char* nackName(uint8_t code) {
    switch (code) {
        case 1: return "crc";
        case 2: return "address";
        case 3: return "sequence";
        case 4: return "verify";
        case 5: return "length";
        default: return "unknown";
    }
}

void otaLog(const String& line) {
    WebSerialConsole::Scope scope(WebSerial, sourceMaster);
    Serial.println(line);
}

// Append one logical byte to the wire buffer, escape-encoded.
void putEscaped(uint8_t* wire, size_t& n, uint8_t b) {
    if (b == kEsc) {
        wire[n++] = kEsc;
        wire[n++] = kEscEsc;
    } else if (b == kForbidden) {
        wire[n++] = kEsc;
        wire[n++] = kEscM;
    } else {
        wire[n++] = b;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Status reporting
// ---------------------------------------------------------------------------

const char* SlaveFlasher::stateString() const {
    switch (state_) {
        case State::Collecting: return "collecting";
        case State::Entering:   return "entering";
        case State::Flashing:   return "flashing";
        case State::Verifying:  return "verifying";
        case State::Done:       return "done";
        case State::Cancelled:  return "cancelled";
        case State::Failed:     return "failed";
        case State::Idle:
        default:                return "idle";
    }
}

int SlaveFlasher::progress() const {
    if (state_ == State::Done) return 100;
    if ((state_ == State::Flashing || state_ == State::Verifying) && totalPages_ > 0) {
        return (int)((uint32_t)pagesDone_ * 100UL / totalPages_);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Image assembly (MQTT "begin" / "chunk")
// ---------------------------------------------------------------------------

bool SlaveFlasher::beginImage(uint8_t moduleId) {
    if (active_ || moduleId == 0) return false;
    collecting_ = true;
    moduleId_ = moduleId;
    size_ = 0;
    b64acc_ = 0;
    b64bits_ = 0;
    error_ = "";
    setState(State::Collecting);
    otaLog("OTA: collecting an image for module " + String((int)moduleId));
    notify();
    return true;
}

bool SlaveFlasher::appendChunk(const char* base64) {
    if (!collecting_ || active_) return false;

    size_t decoded = 0;
    for (const char* p = base64; *p != '\0'; p++) {
        const char c = *p;
        if (c == '=') break;                       // end of padded stream
        if (c == '\r' || c == '\n' || c == ' ') continue;  // tolerate transport cruft

        const int v = base64Value(c);
        if (v < 0) {
            fail("firmware chunk is not valid base64");
            collecting_ = false;
            size_ = 0;
            return false;
        }

        b64acc_ = (b64acc_ << 6) | (uint32_t)v;
        b64bits_ += 6;
        if (b64bits_ >= 8) {
            b64bits_ -= 8;
            if (size_ + decoded >= APP_MAX_SIZE) {
                fail("firmware image exceeds the 14336-byte application region");
                collecting_ = false;
                size_ = 0;
                return false;
            }
            image_[size_ + decoded] = (uint8_t)(b64acc_ >> b64bits_);
            decoded++;
        }
    }

    size_ += decoded;
    return true;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

bool SlaveFlasher::runSession() {
    if (active_) return false;
    if (!imageReady()) {
        fail("no firmware image collected");
        notify();
        return false;
    }

    active_ = true;
    abort_ = false;
    pageAttempted_ = false;
    pagesDone_ = 0;
    bootloaderVersion_ = 0;
    totalPages_ = (uint16_t)((size_ + PAGE_SIZE - 1) / PAGE_SIZE);
    imageCrc_ = 0xFFFFFFFFUL;
    for (uint16_t i = 0; i < size_; i++) imageCrc_ = crc32Byte(imageCrc_, image_[i]);
    imageCrc_ ^= 0xFFFFFFFFUL;
    error_ = "";

    otaLog("OTA: flashing " + String((unsigned)size_) + " bytes (" +
           String((unsigned)totalPages_) + " pages, crc32 " + String(imageCrc_, HEX) +
           ") to module " + String((int)moduleId_));

    bool ok = false;
    const bool appAcked = enterBootloader();
    if (waitForBootloader(appAcked) && startUpdate()) {
        ok = writePages() && finishUpdate();
    }

    if (!ok && abort_) cancelSession(pageAttempted_);
    if (ok) collecting_ = false;  // the image is no longer needed
    active_ = false;

    // Drain stray frame bytes so the main loop's SLAVE console echo does not
    // print binary junk after the session.
    rs485Reset();

    otaLog("OTA: session " + String(ok ? "complete" : stateString() == "failed" ? "failed" : "ended"));
    notify();
    return ok;
}

void SlaveFlasher::cancel() {
    if (active_) {
        // The session checks this between pages and in its wait loops.
        abort_ = true;
        return;
    }
    if (collecting_) {
        collecting_ = false;
        size_ = 0;
        setState(State::Cancelled);
        otaLog("OTA: image collection cancelled");
        notify();
    }
}

bool SlaveFlasher::resetModule(uint8_t moduleId) {
    if (active_ || collecting_) return false;

    active_ = true;
    abort_ = false;
    moduleId_ = moduleId;
    error_ = "";
    setState(State::Entering);
    notify();

    // The module must already sit in the bootloader (boot flag armed or a
    // blank application); a running application never answers binary frames.
    bool found = false;
    for (uint8_t attempt = 0; attempt < MAX_RETRIES && !found; attempt++) {
        Response r;
        if (exchange(kCmdPing, nullptr, 0, r) && r.cmd == kRespAck && r.len == 3 &&
            r.payload[1] == moduleId_) {
            found = true;
        }
    }
    if (!found) {
        fail("module " + String((int)moduleId) + " is not in the bootloader");
        active_ = false;
        notify();
        return false;
    }

    // CMD_RESET clears the boot flag and reboots into the (intact)
    // application.
    Response r;
    sendFrame(kCmdReset, nullptr, 0);
    const bool acked = recvResponse(RESPONSE_TIMEOUT_MS, r) && r.cmd == kRespAck;
    if (acked) {
        setState(State::Done);
        pumpDelay(BOOT_SETTLE_MS);
    } else {
        fail("no acknowledgement of the reset");
    }
    active_ = false;
    rs485Reset();
    notify();
    return acked;
}

// ---------------------------------------------------------------------------
// Session steps
// ---------------------------------------------------------------------------

// Places the ASCII entry command. Returns true when the application
// explicitly acked it; silence is a normal outcome (the module may already
// be in the bootloader, or deaf while a blocking move finishes) and is
// handled by waitForBootloader().
bool SlaveFlasher::enterBootloader() {
    setState(State::Entering);
    notify();
    // If the module is already in the bootloader (flag armed from a
    // previous attempt), skip the ASCII entry command and proceed.
    {
        Response r;
        if (exchange(kCmdPing, nullptr, 0, r) && r.cmd == kRespAck &&
            r.len == 3 && r.payload[1] == moduleId_) {
            otaLog("OTA: module " + String((int)moduleId_) + " already in bootloader");
            pumpDelay(ENTER_BOOT_DELAY_MS);
            return true;
        }
    }
    otaLog("OTA: asking module " + String((int)moduleId_) + " to enter the bootloader");
    rs485Send("m" + String((int)moduleId_) + "b");

    const String reply = readAsciiLine(150);
    const String expect = "m" + String((int)moduleId_) + "b:1";
    if (reply.startsWith(expect)) {
        // The application arms the flag, replies, then resets ~100 ms
        // later; wait out that sequence.
        pumpDelay(ENTER_BOOT_DELAY_MS);
        return true;
    }
    return false;
}

bool SlaveFlasher::waitForBootloader(bool appAcked) {
    const unsigned long deadline = millis() + ENTER_TOTAL_TIMEOUT_MS;

    while ((long)(millis() - deadline) < 0 && !abort_) {
        Response r;
        if (exchange(kCmdPing, nullptr, 0, r) && r.cmd == kRespAck && r.len == 3 &&
            r.payload[1] == moduleId_) {
            if (r.payload[0] != kOtaProtocolVersion) {
                fail("module " + String((int)moduleId_) +
                     " bootloader protocol " + String((int)r.payload[0]) +
                     ", expected " + String((int)kOtaProtocolVersion));
                return false;
            }
            bootloaderVersion_ = r.payload[0];
            // staying == 1: the boot flag is armed (or the application
            // looks erased) — full update mode. staying == 0: the
            // bootloader is in its 500 ms boot window with the flag clear;
            // START_UPDATE arms the flag, so this counts as ready too.
            // The PING just refreshed that window, keeping the module in
            // the bootloader until START_UPDATE arrives milliseconds
            // later.
            otaLog("OTA: bootloader ready (protocol " +
                   String((int)r.payload[0]) +
                   ", staying=" + String((int)r.payload[2]) + ")");
            return true;
        }

        if (!appAcked) {
            // No bootloader answer (yet): the module is either in the
            // application and deaf while a blocking move finishes (it never
            // saw the first request), or the request itself was lost.
            // Re-send it; a module already in update mode ignores the
            // non-frame ASCII bytes harmlessly.
            rs485Send("m" + String((int)moduleId_) + "b");
            if (readAsciiLine(150).startsWith("m" + String((int)moduleId_) + "b:1")) {
                appAcked = true;
                pumpDelay(ENTER_BOOT_DELAY_MS);
                continue;
            }
        }
        pumpDelay(ENTER_RETRY_MS);
    }

    if (abort_) return false;
    fail("module " + String((int)moduleId_) + " did not enter the bootloader");
    return false;
}

bool SlaveFlasher::startUpdate() {
    const uint8_t payload[6] = {
        (uint8_t)(size_ >> 8),
        (uint8_t)(size_ & 0xFF),
        (uint8_t)(imageCrc_ >> 24),
        (uint8_t)(imageCrc_ >> 16),
        (uint8_t)(imageCrc_ >> 8),
        (uint8_t)(imageCrc_ & 0xFF),
    };

    for (uint8_t attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        Response r;
        if (exchange(kCmdStartUpdate, payload, sizeof(payload), r)) {
            if (r.cmd == kRespAck) {
                // The boot flag is armed on the module now: a power cut
                // from this point on re-enters the bootloader.
                setState(State::Flashing);
                notify();
                return true;
            }
            // A NACK here is definitive (length/address rejection);
            // retrying the same frame cannot change it.
            failNack("start update", r.payload[0]);
            return false;
        }
    }
    fail("no acknowledgement of the update start");
    return false;
}

bool SlaveFlasher::writePages() {
    uint8_t payload[2 + PAGE_SIZE];

    for (uint16_t page = 0; page < totalPages_; page++) {
        // runSession() performs the cancellation once the abort is seen
        // here (pageAttempted_ tells it whether the application can still
        // be assumed intact).
        if (abort_) return false;

        payload[0] = (uint8_t)(page >> 8);
        payload[1] = (uint8_t)(page & 0xFF);
        const uint16_t offset = page * PAGE_SIZE;
        const uint16_t chunk = (uint16_t)((size_ - offset) < PAGE_SIZE
                                             ? (size_ - offset)
                                             : PAGE_SIZE);
        memcpy(payload + 2, image_ + offset, chunk);
        if (chunk < PAGE_SIZE) {
            // The tail page is padded with erased-flash bytes; only the
            // image's own bytes count towards the final CRC.
            memset(payload + 2 + chunk, 0xFF, PAGE_SIZE - chunk);
        }

        bool written = false;
        for (uint8_t attempt = 1; attempt <= MAX_RETRIES && !written; attempt++) {
            pageAttempted_ = true;
            Response r;
            if (exchange(kCmdWritePage, payload, sizeof(payload), r)) {
                if (r.cmd == kRespAck) {
                    written = true;
                } else {
                    // NACK: length/sequence/address rejection — definitive.
                    failNack(("page " + String((unsigned)page)).c_str(), r.payload[0]);
                    return false;
                }
            }
            if (!written && attempt < MAX_RETRIES) {
                otaLog("OTA: page " + String((unsigned)page) + " attempt " +
                       String((int)attempt) + " timed out, retrying");
            }
        }
        if (!written) {
            fail("page " + String((unsigned)page) + " was not acknowledged after " +
                 String((int)MAX_RETRIES) + " attempts");
            return false;
        }

        pagesDone_ = page + 1;
        // Progress roughly every 8th page (about once a second) so the
        // status publishes and console lines stay modest.
        if ((pagesDone_ & 7) == 0 || pagesDone_ == totalPages_) notify();
    }
    return true;
}

bool SlaveFlasher::finishUpdate() {
    setState(State::Verifying);
    notify();

    const uint8_t payload[4] = {
        (uint8_t)(imageCrc_ >> 24),
        (uint8_t)(imageCrc_ >> 16),
        (uint8_t)(imageCrc_ >> 8),
        (uint8_t)(imageCrc_ & 0xFF),
    };

    for (uint8_t attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        Response r;
        sendFrame(kCmdFinish, payload, sizeof(payload));
        if (recvResponse(FINISH_TIMEOUT_MS, r)) {
            if (r.cmd == kRespAck) {
                // The slave verified its flash against the image CRC,
                // cleared the boot flag and reset itself into the new
                // application.
                setState(State::Done);
                otaLog("OTA: module " + String((int)moduleId_) +
                       " verified the image and rebooted");
                pumpDelay(BOOT_SETTLE_MS);
                return true;
            }
            // ERR_VERIFY: the flash content does not match the image. The
            // module stays in the bootloader with the boot flag armed; a
            // re-run of "flash" recovers it (the image is kept).
            failNack("final verify", r.payload[0]);
            return false;
        }
    }

    // No ACK. If it was merely lost, the module has already verified,
    // cleared its flag and rebooted — only a successful FINISH (or RESET)
    // clears the flag, so a running application proves success. A fresh
    // boot may auto-home first (blocking, deaf), so probe for a while.
    const unsigned long deadline = millis() + APP_PROBE_TIMEOUT_MS;
    while ((long)(millis() - deadline) < 0 && !abort_) {
        rs485Send("m" + String((int)moduleId_) + "s");
        const String state = readAsciiLine(200);
        if (state.startsWith("m" + String((int)moduleId_) + "s:")) {
            setState(State::Done);
            otaLog("OTA: application is running (the final ACK was lost)");
            pumpDelay(BOOT_SETTLE_MS);
            return true;
        }
        pumpDelay(ENTER_RETRY_MS);
    }

    if (abort_) return false;  // runSession() reports the cancellation
    fail("no acknowledgement of the final verify");
    return false;
}

void SlaveFlasher::cancelSession(bool pagesAttempted) {
    if (!pagesAttempted) {
        // Nothing was placed on the flash: the application is intact, so
        // return the module to it. (The boot flag may be armed from
        // START_UPDATE, which RESET clears again.)
        otaLog("OTA: cancelled before any page was written, resetting the module");
        Response r;
        sendFrame(kCmdReset, nullptr, 0);
        recvResponse(RESPONSE_TIMEOUT_MS, r);  // best effort; it resets on ACK
        pumpDelay(BOOT_SETTLE_MS);
    } else {
        // A page may have been written even if its ACK never arrived, so
        // the application can no longer be assumed intact. Leave the module
        // in the bootloader (its boot flag is armed) — re-running "flash"
        // recovers it, UPDI is the last resort.
        otaLog("OTA: cancelled after pages were written; module stays in the bootloader");
    }
    setState(State::Cancelled);
    notify();
}

// ---------------------------------------------------------------------------
// Wire I/O
// ---------------------------------------------------------------------------

// Sends [0xAA][addr][cmd][len][payload][crc16 hi][crc16 lo] with everything
// after the sync byte escape-encoded. Drives the transceiver directly:
// rs485Send() is for ASCII command lines only (it appends '\n' and its echo
// drop counts exactly that line's bytes). Our own transmission loops back on
// RX in half-duplex; recvResponse() simply skips echoed frames, because a
// valid echo always carries the command code (0x01..0x05) rather than a
// response code (0x81/0x82), so no timing assumptions are needed.
void SlaveFlasher::sendFrame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    uint8_t wire[kWireMax];
    size_t n = 0;
    wire[n++] = kSync;

    uint16_t crc = 0xFFFF;
    crc = crc16Byte(crc, moduleId_);
    crc = crc16Byte(crc, cmd);
    crc = crc16Byte(crc, len);
    for (uint8_t i = 0; i < len; i++) crc = crc16Byte(crc, payload[i]);

    putEscaped(wire, n, moduleId_);
    putEscaped(wire, n, cmd);
    putEscaped(wire, n, len);
    for (uint8_t i = 0; i < len; i++) putEscaped(wire, n, payload[i]);
    putEscaped(wire, n, (uint8_t)(crc >> 8));
    putEscaped(wire, n, (uint8_t)crc);

    digitalWrite(RS485_DE, HIGH);
    delayMicroseconds(100);          // driver settle, as in rs485Send()
    Serial1.write(wire, n);          // blocks until all bytes are queued
    Serial1.flush();                // wait for the last stop bit
    // Drain our own half-duplex echo so recvResponse() only sees the
    // slave's reply.  Without this the echo (which starts with the same
    // 0xAA sync) can be mis-parsed and the real response missed.
    {
        unsigned long echoStart = millis();
        size_t dropped = 0;
        while (dropped < n && (millis() - echoStart) < 5) {
            if (Serial1.available()) {
                Serial1.read();
                dropped++;
            } else {
                yield();
            }
        }
    }
    delayMicroseconds(100);
    digitalWrite(RS485_DE, LOW);
}

// Reads one response frame within timeoutMs. Sync-searches through noise and
// our own echoed transmission, decodes escapes on the fly (an illegal
// sequence resynchronises), verifies CRC-16 and accepts only frames that are
// addressed to us and carry a response code. Pumping keeps MQTT alive (and
// delivers a mid-session "cancel") while the bus is quiet.
bool SlaveFlasher::recvResponse(unsigned long timeoutMs, Response& out) {
    const unsigned long deadline = millis() + timeoutMs;

    bool inSync = false;
    bool escaping = false;
    uint8_t body[kRespBodyMax];
    uint8_t count = 0;

    while ((long)(millis() - deadline) < 0) {
        pump();
        while (Serial1.available() > 0) {
            const uint8_t raw = (uint8_t)Serial1.read();

            if (!inSync) {
                if (raw == kSync) {
                    inSync = true;
                    escaping = false;
                    count = 0;
                }
                continue;
            }

            uint8_t b;
            if (escaping) {
                escaping = false;
                if (raw == kEscEsc) {
                    b = kEsc;
                } else if (raw == kEscM) {
                    b = kForbidden;
                } else {
                    inSync = false;   // illegal escape code
                    continue;
                }
            } else if (raw == kEsc) {
                escaping = true;
                continue;
            } else if (raw == kForbidden) {
                inSync = false;       // 'm' can never appear raw in a frame
                continue;
            } else {
                b = raw;
            }

            if (count >= sizeof(body)) {
                inSync = false;       // overlong frame
                continue;
            }
            body[count++] = b;

            if (count >= 3) {
                const uint8_t len = body[2];
                if (len > kRespMaxPayload) {
                    inSync = false;   // not a response frame
                    continue;
                }
                if (count == (uint8_t)(3 + len + 2)) {
                    // Complete frame: check CRC and ownership.
                    uint16_t crc = 0xFFFF;
                    for (uint8_t i = 0; i < 3 + len; i++) crc = crc16Byte(crc, body[i]);
                    const uint16_t rx = (uint16_t)((body[3 + len] << 8) | body[3 + len + 1]);
                    if (crc == rx && body[0] == moduleId_ &&
                        (body[1] == kRespAck || body[1] == kRespNack)) {
                        out.cmd = body[1];
                        out.len = len;
                        memcpy(out.payload, body + 3, len);
                        return true;
                    }
                    inSync = false;   // CRC mismatch or not ours
                }
            }
        }
        delay(1);   // let WiFi and lower-priority tasks breathe
    }
    return false;
}

bool SlaveFlasher::exchange(uint8_t cmd, const uint8_t* payload, uint8_t len, Response& out) {
    sendFrame(cmd, payload, len);
    return recvResponse(RESPONSE_TIMEOUT_MS, out);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

String SlaveFlasher::readAsciiLine(unsigned long timeoutMs) {
    const unsigned long deadline = millis() + timeoutMs;
    while ((long)(millis() - deadline) < 0) {
        String line = rs485read();
        if (line.length() > 0) return line;
        pump();
        delay(1);
    }
    return "";
}

void SlaveFlasher::pumpDelay(unsigned long ms) {
    const unsigned long deadline = millis() + ms;
    while ((long)(millis() - deadline) < 0) {
        pump();
        delay(1);
    }
}

void SlaveFlasher::fail(const String& message) {
    error_ = message;
    setState(State::Failed);
    otaLog("OTA: " + message);
    notify();
}

void SlaveFlasher::failNack(const char* what, uint8_t code) {
    fail(String(what) + " rejected by the bootloader (nack: " + nackName(code) + ")");
}
