#include <Arduino.h>
#include <WiFi.h>
#include "protocol_handler.h"
#include "secrets.h"
#include <ArduinoOTA.h>

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
const char* ota_ssid = OTA_PASSWORD;

void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  WiFi.setTxPower(WIFI_POWER_15dBm); 
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void setupOTA()
{
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("SplFlp_Master");
  
  ArduinoOTA.setPassword(ota_ssid);
  ArduinoOTA
  .onStart([]() {
    Serial.println("OTA: Update started");
  })
  .onEnd([]() {
    Serial.println("\nOTA: Update successful!");
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r",
                  (progress * 100) / total);
  })
  .onError([](ota_error_t error) {
      Serial.printf("\nOTA Error[%u]: ", error);

      if (error == OTA_AUTH_ERROR) {
          Serial.println("Authentication failed");
      } else if (error == OTA_BEGIN_ERROR) {
          Serial.println("Could not begin update");
      } else if (error == OTA_CONNECT_ERROR) {
          Serial.println("Connection failed");
      } else if (error == OTA_RECEIVE_ERROR) {
          Serial.println("Firmware receive failed");
      } else if (error == OTA_END_ERROR) {
          Serial.println("Update finalization failed");
      } else {
          Serial.println("Unknown OTA error");
      }
    });
  ArduinoOTA.begin();
  return;
}