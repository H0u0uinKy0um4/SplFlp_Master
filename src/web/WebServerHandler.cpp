#include "web/WebServerHandler.h"
#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "web/WebSerialConsole.h"
#include "web/index_html.h"

static WebServer server(80);
static WebSocketsServer webSocket(81);

void setupWebServer() {
    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    });

    server.onNotFound([]() {
        Serial.print("Unknown HTTP request: ");
        Serial.println(server.uri());
        server.send(404, "text/plain", "Not found: " + server.uri());
    });

    server.begin();
    webSocket.begin();
    WebSerial.attachWebSocket(&webSocket);
    Serial.print("Web Serial Console ready at: http://");
}

void handleWebServer() {
    server.handleClient();
    WebSerial.loop();
}
