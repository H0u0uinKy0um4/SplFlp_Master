#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WebSerialConsole.h>
#include <WiFi.h>
#include "BusTrace.h"
#include "ModuleRuntime.h"
#include "Rs485Link.h"
#include "network/OtaHandler.h"
#include "network/WifiMqttHandler.h"

void setup() {
    Serial.begin(115200);
    delay(500);

    // Register console sources before anything is logged.
    busTraceBegin();

    rs485Begin();

    // Non-blocking WiFi start: the device still boots when the router is not
    // up yet, and associates later from handleMqtt().
    beginWifi();
    setupMqtt();
    setupOTA();
    WebSerial.beginWeb();
    moduleRuntime.begin();
}

void loop() {
    ArduinoOTA.handle();
    // Serves the console page, pumps the WebSocket and the serial bridge.
    WebSerial.loop();
    handleMqtt();

    // Manual commands from USB Serial enter the same RS485 Link as MQTT
    // commands; no second reader competes for Serial1 bytes.
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) rs485SendLine(line);
    }

    yield();
}
