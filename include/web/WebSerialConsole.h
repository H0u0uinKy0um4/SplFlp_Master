#pragma once
#include <Arduino.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <cstring>

#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
#if ARDUINO_USB_CDC_ON_BOOT
#include "HWCDC.h"
#endif
#endif

namespace WebSerialDetail {
    inline decltype(::Serial)& getHWSerial() {
        return ::Serial;
    }
}

class WebSerialConsole : public Stream {
public:
    typedef std::function<bool(const String& cmd)> CommandHandler;

    // Output categories. The numeric order is also used as the index of the
    // per-source line buffers, so keep it stable.
    enum class Source : uint8_t {
        DEBUG = 0,
        HA    = 1,
        USER  = 2,
        SLAVE = 3
    };
    static constexpr uint8_t SOURCE_COUNT = 4;

    // Temporarily tag everything written through Serial with a source.
    class Scope {
    public:
        Scope(WebSerialConsole& console, Source source)
            : _console(console), _previous(console.source()) {
            _console.setSource(source);
        }
        ~Scope() { _console.setSource(_previous); }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        WebSerialConsole& _console;
        Source _previous;
    };

private:
    WebSocketsServer* _ws = nullptr;
    CommandHandler _customCmdHandler = nullptr;

    static const size_t LINE_BUF_SIZE = 256;
    static const size_t SCROLLBACK_SIZE = 4096;

    Source _source = Source::DEBUG;

    // One partially received line per source, so output from different
    // sources can never be mixed into a single terminal line.
    char _lineBuffer[SOURCE_COUNT][LINE_BUF_SIZE];
    size_t _lineLen[SOURCE_COUNT] = {};
    unsigned long _lineLastMs[SOURCE_COUNT] = {};

    // Incoming RX ring buffer (fed by WebSocket or USB CDC)
    static const size_t RX_BUF_SIZE = 512;
    uint8_t _rxBuffer[RX_BUF_SIZE];
    volatile size_t _rxHead = 0;
    volatile size_t _rxTail = 0;

    // Recent output scrollback. Stored as already-tagged lines, so it can be
    // replayed verbatim to a newly connected client.
    char _scrollback[SCROLLBACK_SIZE];
    size_t _scrollbackLen = 0;

public:
    WebSerialConsole() {}

    void begin(unsigned long baud = 115200) {
        WebSerialDetail::getHWSerial().begin(baud);
    }

    void attachWebSocket(WebSocketsServer* ws) {
        _ws = ws;
        if (_ws) {
            _ws->onEvent([this](uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
                this->handleWebSocketEvent(num, type, payload, length);
            });
        }
    }

    void onCommand(CommandHandler handler) {
        _customCmdHandler = handler;
    }

    Source source() const { return _source; }
    void setSource(Source source) { _source = source; }

    // Emit one complete line under the given source. Also goes to the
    // hardware UART so the USB console stays in sync.
    void logLine(Source source, const String& text) {
        Scope scope(*this, source);
        write((const uint8_t*)text.c_str(), text.length());
        write('\n');
    }

    // Stream & Print interface implementation
    virtual size_t write(uint8_t c) override {
        // 1. Output to physical hardware USB serial port immediately
        WebSerialDetail::getHWSerial().write(c);

        // 2. Buffer the character for the WebSocket line currently being built
        emitChar((char)c);
        return 1;
    }

    virtual size_t write(const uint8_t *buffer, size_t size) override {
        if (!buffer || size == 0) return 0;
        for (size_t i = 0; i < size; i++) {
            write(buffer[i]);
        }
        return size;
    }

    virtual int available() override {
        size_t rxCount = (_rxHead >= _rxTail) ? (_rxHead - _rxTail) : (RX_BUF_SIZE + _rxHead - _rxTail);
        return (int)(rxCount + WebSerialDetail::getHWSerial().available());
    }

    virtual int read() override {
        if (_rxHead != _rxTail) {
            uint8_t b = _rxBuffer[_rxTail];
            _rxTail = (_rxTail + 1) % RX_BUF_SIZE;
            return b;
        }
        return WebSerialDetail::getHWSerial().read();
    }

    virtual int peek() override {
        if (_rxHead != _rxTail) {
            return _rxBuffer[_rxTail];
        }
        return WebSerialDetail::getHWSerial().peek();
    }

    virtual void flush() override {
        WebSerialDetail::getHWSerial().flush();
    }

    operator bool() const {
        return (bool)WebSerialDetail::getHWSerial();
    }

    // Main service loop: handle websockets and forward USB CDC input.
    void loop() {
        if (_ws) {
            _ws->loop();
        }

        // Flush stale partial lines (a print without a trailing newline).
        // USER is excluded: slow typing on the hardware console must stay in
        // one line until Enter is pressed.
        unsigned long now = millis();
        for (uint8_t i = 0; i < SOURCE_COUNT; i++) {
            if ((Source)i == Source::USER) continue;
            if (_lineLen[i] > 0 && now - _lineLastMs[i] >= 25) {
                flushLine((Source)i);
            }
        }

        // Everything typed on the hardware serial console belongs to the
        // USER category and must also be readable through Serial.read().
        if (WebSerialDetail::getHWSerial().available()) {
            Scope scope(*this, Source::USER);
            while (WebSerialDetail::getHWSerial().available()) {
                int c = WebSerialDetail::getHWSerial().read();
                if (c >= 0) {
                    emitChar((char)c);
                    pushRx((uint8_t)c);
                }
            }
        }
    }

private:
    static char tagFor(Source source) {
        switch (source) {
            case Source::HA:    return 'H';
            case Source::USER:  return 'U';
            case Source::SLAVE: return 'S';
            default:            return 'D';
        }
    }

    void emitChar(char c) {
        uint8_t idx = (uint8_t)_source;
        if (idx >= SOURCE_COUNT) idx = 0;

        if (c == '\r' || c == '\n') {
            if (_lineLen[idx] > 0) flushLine((Source)idx);
            return;
        }

        if (_lineLen[idx] >= LINE_BUF_SIZE - 1) {
            flushLine((Source)idx);
        }

        _lineBuffer[idx][_lineLen[idx]++] = c;
        _lineLastMs[idx] = millis();
    }

    void flushLine(Source source) {
        uint8_t idx = (uint8_t)source;
        size_t len = _lineLen[idx];
        if (len == 0) return;

        _lineBuffer[idx][len] = '\0';
        broadcastTagged(source, _lineBuffer[idx], len);
        _lineLen[idx] = 0;
    }

    // Wire format: 0x01 <tag> <line text> '\n'
    //   H = Home Assistant request, U = user command,
    //   S = slave response, D = debug output.
    void broadcastTagged(Source source, const char* text, size_t len) {
        char out[LINE_BUF_SIZE + 3];
        out[0] = '\x01';
        out[1] = tagFor(source);
        memcpy(out + 2, text, len);
        out[len + 2] = '\n';
        size_t total = len + 3;

        appendToScrollback(out, total);

        if (_ws && _ws->connectedClients() > 0) {
            _ws->broadcastTXT((uint8_t*)out, total);
        }
    }

    void appendToScrollback(const char* data, size_t len) {
        if (len == 0) return;

        if (len >= SCROLLBACK_SIZE) {
            data += len - (SCROLLBACK_SIZE - 1);
            len = SCROLLBACK_SIZE - 1;
        }

        if (_scrollbackLen + len > SCROLLBACK_SIZE - 1) {
            size_t overflow = _scrollbackLen + len - (SCROLLBACK_SIZE - 1);
            size_t drop = overflow;
            // Do not start the replay in the middle of a tagged line.
            while (drop < _scrollbackLen && _scrollback[drop - 1] != '\n') drop++;
            if (drop > _scrollbackLen) drop = _scrollbackLen;

            memmove(_scrollback, _scrollback + drop, _scrollbackLen - drop);
            _scrollbackLen -= drop;
        }

        memcpy(_scrollback + _scrollbackLen, data, len);
        _scrollbackLen += len;
    }

    String taggedLine(Source source, const String& text) {
        String out;
        out.reserve(text.length() + 3);
        out += (char)0x01;
        out += tagFor(source);
        out += text;
        out += '\n';
        return out;
    }

    void pushRx(uint8_t b) {
        size_t next = (_rxHead + 1) % RX_BUF_SIZE;
        if (next != _rxTail) {
            _rxBuffer[_rxHead] = b;
            _rxHead = next;
        }
    }

    void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
        switch (type) {
            case WStype_DISCONNECTED: {
                logLine(Source::DEBUG, "[WebSerial] Client #" + String(num) + " disconnected");
                break;
            }
            case WStype_CONNECTED: {
                IPAddress ip = _ws->remoteIP(num);
                // Keep this off the web stream: the client receives the same
                // information in its welcome banner below.
                WebSerialDetail::getHWSerial().printf(
                    "[WebSerial] Client #%u connected from %s\n", num, ip.toString().c_str());

                // Welcome message, tagged as debug/system output
                String banner;
                banner += taggedLine(Source::DEBUG, "=== Connected to ESP32-C3 Web Serial Console ===");
                banner += taggedLine(Source::DEBUG,
                                     "IP: " + WiFi.localIP().toString() +
                                     " | Free Heap: " + String(ESP.getFreeHeap()) + " B");
                banner += taggedLine(Source::DEBUG, "Type 'help' for available commands.");
                banner += taggedLine(Source::DEBUG, "=================================================");
                _ws->sendTXT(num, banner);

                // Send recent scrollback if available
                if (_scrollbackLen > 0) {
                    _ws->sendTXT(num, (const uint8_t*)_scrollback, _scrollbackLen);
                }
                break;
            }
            case WStype_TEXT: {
                if (length == 0 || !payload) break;

                String text = "";
                text.reserve(length);
                for (size_t i = 0; i < length; i++) {
                    text += (char)payload[i];
                }

                String cmd = text;
                cmd.trim();

                // Echo the typed command to every client (including this one)
                // as a USER line, so it is filtered/exported like any other.
                if (cmd.length() > 0) {
                    logLine(Source::USER, cmd);
                }

                if (cmd.equalsIgnoreCase("help")) {
                    println("\r\n--- Built-in Console Commands ---");
                    println("  help    : Show this help list");
                    println("  ip      : Display WiFi connection details");
                    println("  heap    : Display free RAM / heap stats");
                    println("  uptime  : Display ESP32 uptime");
                    println("  restart : Reboot the ESP32");
                    println("---------------------------------");
                    return;
                } else if (cmd.equalsIgnoreCase("ip")) {
                    println("\r\n--- WiFi Info ---");
                    print("  IP Address : "); println(WiFi.localIP());
                    print("  Subnet Mask: "); println(WiFi.subnetMask());
                    print("  Gateway    : "); println(WiFi.gatewayIP());
                    print("  DNS        : "); println(WiFi.dnsIP());
                    print("  RSSI       : "); print(WiFi.RSSI()); println(" dBm");
                    print("  MAC        : "); println(WiFi.macAddress());
                    println("-----------------");
                    return;
                } else if (cmd.equalsIgnoreCase("heap")) {
                    println("\r\n--- Memory Info ---");
                    print("  Free Heap     : "); print(ESP.getFreeHeap()); println(" bytes");
                    print("  Min Free Heap : "); print(ESP.getMinFreeHeap()); println(" bytes");
                    print("  Max Alloc Heap: "); print(ESP.getMaxAllocHeap()); println(" bytes");
                    println("-------------------");
                    return;
                } else if (cmd.equalsIgnoreCase("uptime")) {
                    unsigned long sec = millis() / 1000;
                    unsigned long m = sec / 60;
                    unsigned long h = m / 60;
                    unsigned long d = h / 24;
                    println("\r\n--- System Uptime ---");
                    printf("  %lu days, %02lu:%02lu:%02lu\n", d, h % 24, m % 60, sec % 60);
                    println("---------------------");
                    return;
                } else if (cmd.equalsIgnoreCase("restart") || cmd.equalsIgnoreCase("reboot")) {
                    println("\r\n[WebSerial] Rebooting ESP32 in 500ms...");
                    flush();
                    delay(500);
                    ESP.restart();
                    return;
                }

                // Custom command handler if registered
                if (_customCmdHandler && _customCmdHandler(cmd)) {
                    return;
                }

                // Push incoming bytes into RX buffer so standard Serial.read() can consume them
                for (size_t i = 0; i < length; i++) {
                    pushRx(payload[i]);
                }
                break;
            }
            default:
                break;
        }
    }
};

extern WebSerialConsole WebSerial;

#ifndef NO_WEB_SERIAL_REDEFINE
#define Serial WebSerial
#endif
