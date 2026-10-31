#include <Arduino.h>
#include "network/OtaHandler.h"
#include "secrets.h"
#include <ArduinoOTA.h>
#include <PubSubClient.h>


const char* ota_password = OTA_PASSWORD;


void setupOTA()
{
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("SplFlp_Master");
  
  ArduinoOTA.setPassword(ota_password);
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


