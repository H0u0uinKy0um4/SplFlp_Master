#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include "web/WebSerialConsole.h"
#include "network/OtaHandler.h"
#include "network/WifiMqttHandler.h"
#include "web/WebServerHandler.h"
#include "ProtocolLayer.h"
#include "RS485.h"
#include "Display.h"

void setup() {
  Serial.begin(115200);
  delay(500);

  setupRS485();
  connectWifi();
  setupMqtt();
  setupOTA();
  setupWebServer();
  splitflap.begin();
}

void loop() {
  ArduinoOTA.handle();
  handleWebServer();
  handleMqtt();

// Send commands from Serial to RS485
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    rs485Send(line);
  }

  // Read Attiny response
  if (Serial1.available()) {
    WebSerialConsole::Scope scope(WebSerial, WebSerialConsole::Source::SLAVE);
    while (Serial1.available()) {
      char c = Serial1.read();
      Serial.write(c);
    }
  }

  // Yield CPU to FreeRTOS watchdog & background Wi-Fi tasks
  yield();
}
