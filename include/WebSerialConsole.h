#pragma once
#include <Arduino.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

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

private:
    WebSocketsServer* _ws = nullptr;
    CommandHandler _customCmdHandler = nullptr;

    // Outgoing buffer for WebSocket broadcast
    static const size_t TX_BUF_SIZE = 512;
    char _txBuffer[TX_BUF_SIZE];
    size_t _txLen = 0;
    unsigned long _lastTxTime = 0;

    // Incoming RX ring buffer (fed by WebSocket or USB CDC)
    static const size_t RX_BUF_SIZE = 512;
    uint8_t _rxBuffer[RX_BUF_SIZE];
    volatile size_t _rxHead = 0;
    volatile size_t _rxTail = 0;

    // Recent output scrollback for newly connected clients
    static const size_t SCROLLBACK_SIZE = 2048;
    char _scrollback[SCROLLBACK_SIZE];
    size_t _scrollbackLen = 0;

public:
    WebSerialConsole() : _txLen(0), _lastTxTime(0), _rxHead(0), _rxTail(0), _scrollbackLen(0) {}

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

    // Stream & Print interface implementation
    virtual size_t write(uint8_t c) override {
        // 1. Output to physical hardware USB serial port immediately
        WebSerialDetail::getHWSerial().write(c);

        // 2. Save in recent scrollback buffer
        appendToScrollback((char)c);

        // 3. Buffer for WebSocket broadcast
        _txBuffer[_txLen++] = (char)c;
        if (_txLen >= TX_BUF_SIZE - 1 || c == '\n') {
            flushWs();
        }
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
        flushWs();
    }

    operator bool() const {
        return (bool)WebSerialDetail::getHWSerial();
    }

    // Main service loop: handle websockets, read hardware serial, flush TX
    void loop() {
        if (_ws) {
            _ws->loop();
        }

        // Forward any data typed in hardware Serial (USB CDC) to WebSocket and RX buffer
        while (WebSerialDetail::getHWSerial().available()) {
            int c = WebSerialDetail::getHWSerial().read();
            if (c >= 0) {
                appendToScrollback((char)c);
                _txBuffer[_txLen++] = (char)c;
                if (_txLen >= TX_BUF_SIZE - 1 || c == '\n') {
                    flushWs();
                }
                pushRx((uint8_t)c);
            }
        }

        // Periodically flush any un-flushed characters after 25ms
        if (_txLen > 0 && (millis() - _lastTxTime >= 25)) {
            flushWs();
        }
    }

private:
    void appendToScrollback(char c) {
        if (_scrollbackLen < SCROLLBACK_SIZE - 1) {
            _scrollback[_scrollbackLen++] = c;
        } else {
            const size_t shift = 512;
            memmove(_scrollback, _scrollback + shift, SCROLLBACK_SIZE - shift);
            _scrollbackLen = SCROLLBACK_SIZE - shift;
            _scrollback[_scrollbackLen++] = c;
        }
    }

    void pushRx(uint8_t b) {
        size_t next = (_rxHead + 1) % RX_BUF_SIZE;
        if (next != _rxTail) {
            _rxBuffer[_rxHead] = b;
            _rxHead = next;
        }
    }

    void flushWs() {
        if (_txLen > 0 && _ws != nullptr && _ws->connectedClients() > 0) {
            _ws->broadcastTXT((uint8_t*)_txBuffer, _txLen);
        }
        _txLen = 0;
        _lastTxTime = millis();
    }

    void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
        switch (type) {
            case WStype_DISCONNECTED: {
                WebSerialDetail::getHWSerial().printf("[WebSerial] Client #%u disconnected\n", num);
                break;
            }
            case WStype_CONNECTED: {
                IPAddress ip = _ws->remoteIP(num);
                WebSerialDetail::getHWSerial().printf("[WebSerial] Client #%u connected from %s\n", num, ip.toString().c_str());

                // Welcome message
                String banner = "\r\n=== Connected to ESP32-C3 Web Serial Console ===\r\n";
                banner += "IP: " + WiFi.localIP().toString() + " | Free Heap: " + String(ESP.getFreeHeap()) + " B\r\n";
                banner += "Type 'help' for available commands.\r\n";
                banner += "=================================================\r\n";
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

                WebSerialDetail::getHWSerial().printf("[WebConsole IN]: %s\n", text.c_str());

                String cmd = text;
                cmd.trim();

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
