#include "Rs485Link.h"
#include "BusTrace.h"
#include "Config.h"

namespace {

constexpr size_t RX_LINE_MAX = 2048;

String rxLine;
unsigned long rxLastByteMs = 0;

void driverOn() {
    digitalWrite(RS485_DE, HIGH);
    delayMicroseconds(100);
}

void driverOff() {
    delayMicroseconds(100);
    digitalWrite(RS485_DE, LOW);
}

// Half-duplex transceivers loop our own transmission back on RX. Drop exactly
// the bytes we sent: draining everything would throw away a fast reply.
void dropEcho(size_t sentBytes) {
    const unsigned long start = millis();
    size_t dropped = 0;
    while (dropped < sentBytes && (millis() - start) < 5) {
        if (Serial1.available()) {
            Serial1.read();
            dropped++;
        } else {
            yield();
        }
    }
}

}  // namespace

void rs485Begin() {
    pinMode(RS485_DE, OUTPUT);
    digitalWrite(RS485_DE, LOW);
    Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
    Serial.println("RS485 master ready");
}

void rs485Reset() {
    rxLine = "";
    rxLastByteMs = 0;
    while (Serial1.available()) Serial1.read();
}

bool rs485RxBusy() {
    return rxLine.length() > 0;
}

void rs485SendLine(const String& line) {
    busTraceLine(BusSource::Master, "→ " + line);
    rs485Reset();

    const bool addTerminator = !line.endsWith("\n");
    const size_t sentBytes = line.length() + (addTerminator ? 1 : 0);

    driverOn();
    Serial1.print(line);
    if (addTerminator) Serial1.print('\n');
    Serial1.flush();
    dropEcho(sentBytes);
    driverOff();
}

String rs485ReadLine(bool trim) {
    if (Serial1.available() <= 0) {
        // The slave terminates with '\n', but a missing terminator must not
        // stall a caller once the bus has gone quiet.
        if (rxLine.length() > 0 && millis() - rxLastByteMs > 20) {
            String line = rxLine;
            rxLine = "";
            if (trim) line.trim();
            return line;
        }
        return "";
    }

    const int toRead = min(Serial1.available(), 128);
    for (int i = 0; i < toRead; i++) {
        const char value = (char)Serial1.read();
        rxLastByteMs = millis();
        busTraceChar(BusSource::Slave, value);

        if (value == '\n' || value == '\r') {
            if (rxLine.length() == 0) continue;
            String line = rxLine;
            rxLine = "";
            if (trim) line.trim();
            return line;
        }

        rxLine += value;
        if (rxLine.length() >= RX_LINE_MAX) rxLine = "";
    }
    return "";
}

void rs485SendFrame(const uint8_t* frame, size_t length) {
    busTraceLine(BusSource::Master,
                 "→ OTA frame (" + String((unsigned)length) + " bytes)");
    driverOn();
    Serial1.write(frame, length);
    Serial1.flush();
    dropEcho(length);
    driverOff();
}

size_t rs485Available() {
    return (size_t)Serial1.available();
}

int rs485ReadByte() {
    if (!Serial1.available()) return -1;
    return Serial1.read();
}
