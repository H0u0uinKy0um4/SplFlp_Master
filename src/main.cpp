#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "WebSerialConsole.h"
#include "index_html.h"
#include "protocol_handler.h"


#define RS485_RX 5
#define RS485_TX 6
#define RS485_DE 7
#define RS485_BAUD 9600

WebServer server(80);
WebSocketsServer webSocket(81);


void rs485Send(const String &msg) {
  digitalWrite(RS485_DE, HIGH);
  delayMicroseconds(100);
  Serial1.print(msg);
  Serial1.flush();
  delayMicroseconds(100);
  digitalWrite(RS485_DE, LOW);
  while (Serial1.available()) Serial1.read();  // zahodit ozvěnu
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(1000);

  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);

  connectWifi();
  setupOTA();

  // Define HTTP routes
  server.on("/", HTTP_GET, []() {server.send_P(200,"text/html; charset=utf-8",
    INDEX_HTML);});
  server.onNotFound([](){Serial.print("Unknown HTTP request: ");
    Serial.println(server.uri());
    server.send(404, "text/plain", "Not found: " + server.uri());});

  server.begin();
  webSocket.begin();
  WebSerial.attachWebSocket(&webSocket);

  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);

  Serial.println("RS485 master ready");
  Serial.print("Web Serial Console ready at: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  WebSerial.loop();

  // Send commands from Serial to RS485
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    rs485Send(line);
  }

  //Read Attiny responce
  while (Serial1.available()) {
    char c = Serial1.read();
    Serial.write(c);
  }


}