#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebSerialConsole.h>
#include "Sources.h"
#include "network/OtaHandler.h"
#include "network/WifiMqttHandler.h"
#include "RS485.h"
#include "Display.h"

void setup() {
  Serial.begin(115200);
  delay(500);

  // Register the output categories before anything is logged, so the web
  // console can label and colour them.
  registerSources();

  setupRS485();
  // Non-blocking WiFi start: if the router is not up yet (e.g. after a power
  // cut), the device still boots fully and associates later from the main
  // loop. The web console, OTA and RS485 keep working meanwhile.
  beginWifi();
  setupMqtt();
  setupOTA();
  // Start the web console (HTTP + WebSocket). WiFi is owned by beginWifi().
  WebSerial.beginWeb();
  splitflap.begin();
}

void loop() {
  ArduinoOTA.handle();
  // Serves the console page, pumps the WebSocket and the serial bridge.
  WebSerial.loop();
  handleMqtt();

// Send commands from Serial to RS485
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    rs485Send(line);
  }

  // Read Attiny response
  if (Serial1.available()) {
    WebSerialConsole::Scope scope(WebSerial, sourceSlave);
    while (Serial1.available()) {
      char c = Serial1.read();
      Serial.write(c);
    }
  }

  // Yield CPU to FreeRTOS watchdog & background Wi-Fi tasks
  yield();
}
