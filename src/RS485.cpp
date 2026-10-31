#include <Arduino.h>
#include <esp_system.h>
#include "RS485.h"
#include "Config.h"
#include "web/WebSerialConsole.h"

// Non-blocking serial command buffer
static String serialCmdBuffer = "";

// Partially received response line
static String rxLine = "";
static unsigned long rxLastByteMs = 0;

// A module config response can contain up to 64 position-map entries, so the
// receive buffer must be much larger than one short command line.
static constexpr size_t RX_LINE_MAX = 2048;

void setupRS485()
{
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
  Serial.println("RS485 master readyS");
}

void rs485Reset()
{
  rxLine = "";
  rxLastByteMs = 0;
  while (Serial1.available()) Serial1.read();
}



void rs485Send(const String &msg) {
  rs485Reset();
  // The slave parser terminates on '\n' (or an idle gap). Always send an
  // explicit terminator so it reacts immediately instead of waiting for the
  // 50 ms silence timeout.
  const bool addTerminator = !msg.endsWith("\n");
  const size_t sentBytes = msg.length() + (addTerminator ? 1 : 0);

  digitalWrite(RS485_DE, HIGH);
  delayMicroseconds(100);

  Serial1.print(msg);
  if (addTerminator) Serial1.print('\n');

  // Wait until the last stop bit has actually left the shift register, not
  // just until the TX FIFO is empty.
  Serial1.flush();

  // In half-duplex mode the transceiver loops our own transmission back on
  // RX. Drop exactly the bytes we sent. Draining everything that happens to
  // be available is dangerous: a slave that answers quickly would have the
  // first bytes of its response thrown away.
  unsigned long echoStart = millis();
  size_t dropped = 0;
  while (dropped < sentBytes && (millis() - echoStart) < 5) {
    if (Serial1.available()) {
      Serial1.read();
      dropped++;
    } else {
      yield();
    }
  }

  digitalWrite(RS485_DE, LOW);
}


// Bounded read from RS-485 to prevent Watchdog starvation
String rs485read(bool trim) {
  // Echo every received byte live to the console as a SLAVE line, so the
  // response is visible even if the line never terminates or never parses.
  WebSerialConsole::Scope scope(WebSerial, WebSerialConsole::Source::SLAVE);

  int availableBytes = Serial1.available();

  if (availableBytes <= 0) {
    // The slave terminates with '\n', but return a partial line if the bus
    // has gone quiet so a missing terminator cannot stall the caller.
    if (rxLine.length() > 0 && millis() - rxLastByteMs > 20) {
      String line = rxLine;
      rxLine = "";
      if (trim) line.trim();
      return line;
    }
    return "";
  }

  // Process up to 128 bytes per loop pass
  int toRead = min(availableBytes, 128);

  for (int i = 0; i < toRead; i++) {
    char c = Serial1.read();
    rxLastByteMs = millis();

    Serial.print(c);

    if (c == '\n' || c == '\r') {
      if (rxLine.length() == 0) continue;
      String receivedCommand = rxLine;
      rxLine = "";
      if (trim) receivedCommand.trim();
      return receivedCommand;
    }

    rxLine += c;

    // Prevent unlimited buffer growth
    if (rxLine.length() >= RX_LINE_MAX) {
      rxLine = "";
    }
  }

  return "";
}

bool rs485RxBusy()
{
  return rxLine.length() > 0;
}
