#include "network/WifiMqttHandler.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "web/WebSerialConsole.h"
#include "Display.h"


const char *wifi_ssid = WIFI_SSID;
const char *wifi_password = WIFI_PASSWORD;
const char *mqtt_password = MQTT_PASSWORD;
const char *mqtt_host = MQTT_HOST;
const char *mqtt_user = MQTT_USER;
const int mqtt_port = 1883; // Standard MQTT port
const char* const MQTT_CLIENT_ID = "SplitFlapMaster";

const char* topic_prefix = "splitflap";
String status_topic  = String(topic_prefix) + "/status";
String display_topic = String(topic_prefix) + "/command/display";
String sync_topic    = String(topic_prefix) + "/command/sync";
String scan_topic    = String(topic_prefix) + "/command/scan";
String update_topic  = String(topic_prefix) + "/command/update";

WiFiClient wifiClient;
PubSubClient client(wifiClient);
String current_text = "";
unsigned long last_status_publish = 0;

// Grows with the status payload so the per-module diagnostics (drum and
// position map) are not truncated by the MQTT client's buffer.
size_t mqtt_buffer_size = 2048;

// Send a periodic heartbeat so the Home Assistant integration does not mark the
// device offline while it is idle.
constexpr unsigned long STATUS_PUBLISH_INTERVAL_MS = 30000;

// Last Will: if the ESP32 drops off the broker, Home Assistant receives an
// offline state instead of waiting for its inactivity timeout.
constexpr char OFFLINE_WILL_PAYLOAD[] =
    "{\"state\":\"offline\",\"modules\":[],\"modules_count\":0}";


// The SplitFlap Home Assistant integration creates all entities itself (master
// status, module count and one sensor per module). Firmware-side discovery only
// duplicates those entities, so it is disabled by default. Enable it with
//   -DSPLFLAP_ENABLE_MQTT_DISCOVERY=1
// when the device is used without the integration.
#ifndef SPLFLAP_ENABLE_MQTT_DISCOVERY
#define SPLFLAP_ENABLE_MQTT_DISCOVERY 0
#endif

namespace {
constexpr char DISCOVERY_TOPIC[] =
    "homeassistant/sensor/splflp_master_status/config";
// Older firmware builds published a second discovery config with the same
// unique_id on this topic. While retained, Home Assistant logs
// "ID splflp_master_status already exists - ignoring ...".
constexpr char LEGACY_DEVICE_DISCOVERY_TOPIC[] =
    "homeassistant/device/splflp_master/config";
constexpr char DISCOVERY_UNIQUE_ID[] = "splflp_master_status";

// Placeholder modules until the RS485 scan is wired into the MQTT status
// payload. Keeping them in one table guarantees that `modules_count` always
// matches the `modules` array. Replace these values with the response of the
// slave `m<id>d` (config) and `m<id>l` (drum) commands.
};

void publishDiscovery() {
  // Always delete the obsolete retained payload from older firmware builds.
  client.publish(LEGACY_DEVICE_DISCOVERY_TOPIC, "", true);

#if SPLFLAP_ENABLE_MQTT_DISCOVERY
  JsonDocument doc;
  doc["name"] = "SplitFlap Status";
  doc["state_topic"] = status_topic;
  doc["unique_id"] = DISCOVERY_UNIQUE_ID;
  doc["value_template"] = "{{ value_json.state }}";

  JsonObject device = doc["device"].to<JsonObject>();

  // identifiers must be an array in Home Assistant MQTT discovery.
  JsonArray identifiers = device["identifiers"].to<JsonArray>();
  identifiers.add("splflp_master");

  device["name"] = "SplitFlap Master";
  device["model"] = "ESP32-C3 RS485 Master";
  device["manufacturer"] = "DIY";

  char buffer[512];
  serializeJson(doc, buffer);

  client.publish(DISCOVERY_TOPIC, buffer, true);
  Serial.println("Published MQTT Discovery configuration!");
#else
  // Clear a previously retained firmware discovery entry so the integration
  // remains the single owner of the Home Assistant entities.
  client.publish(DISCOVERY_TOPIC, "", true);
#endif
}

//WIFI Connection
void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  WiFi.setTxPower(WIFI_POWER_15dBm); 
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}



void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Everything logged from an MQTT callback is an HA-originated request.
  WebSerialConsole::Scope scope(WebSerial, WebSerialConsole::Source::HA);

  String incomingTopic = String(topic);

  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

    // DISPLAY COMMAND
  if (incomingTopic == display_topic) {

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, message);

      if (!error) {
          JsonArray moduleArray = doc["modules"].as<JsonArray>();

          if (!moduleArray.isNull()) {
              String assembled;
              for (JsonObject module : moduleArray) {
                  int id = module["id"] | -1;
                  const char* value = module["char"] | "";
                  char flap = value[0];

                  if (id <= 0 || flap == '\0') {
                      continue;
                  }
                  if (flap == '\r' || flap == '\n') {
                      flap = ' ';
                  }

                  splitflap.setModuleChar(static_cast<uint8_t>(id), flap);
                  assembled += flap;
              }

              current_text = assembled;
              Serial.println("Received per-module display: " + current_text);

              // Report the requested characters immediately; the motors keep
              // turning in the background.
              publishStatus();
          }
          // Backwards compatibility for clients that still send a string.
          else if (doc["text"].is<const char*>()) {
              current_text = doc["text"].as<String>();

              Serial.println("Received Display Text: " + current_text);

              // TODO: Send text to physical display
              // splitflap.displayText(current_text);

              publishStatus();
          }
      }
  }

    // SYNC COMMAND
    else if (incomingTopic == sync_topic) {

        Serial.println("Sync requested from Home Assistant!");

        // Publish current real module data
        publishStatus();
    }

    // SCAN COMMAND
    else if (incomingTopic == scan_topic) {

        Serial.println("Bus scan requested from Home Assistant!");

        // Discover modules over RS485. The first argument is the start ID, so
        // pass the scan range explicitly instead of the timeout constant.
        size_t found = splitflap.discoverModules(ID_BEGIN, ID_END);

        Serial.printf("Found %u modules\n", (unsigned)found);

        // Publish newly discovered modules
        publishStatus();
    }

    // UPDATE COMMAND
    else if (incomingTopic == update_topic) {

        Serial.println("Module update requested from Home Assistant!");

        // Refresh only the modules that are already known. Unlike a scan this
        // does not clear the module list or walk every possible ID, so it is
        // fast and safe to call frequently.
        splitflap.updateModules();

        publishStatus();
    }
}

// Setup MQTT Server and Callback
void setupMqtt() {
  // The status payload includes per-module diagnostics (flap list, position
  // map), so it no longer fits in the default 256-byte buffer.
  client.setBufferSize(2048);
  client.setServer(mqtt_host, mqtt_port);
  client.setCallback(mqttCallback);
  Serial.println("setUp Mqtt");
}

// Connect/Reconnect to MQTT Broker
void connectMqtt() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // Attempt to connect with Client ID, User, Password and a retained LWT.
    if (client.connect(MQTT_CLIENT_ID, mqtt_user, mqtt_password,
                       status_topic.c_str(), 1, true, OFFLINE_WILL_PAYLOAD)) {
      Serial.println("connected!");
      
      // Resubscribe to command topics upon successful connection
      client.subscribe(display_topic.c_str());
      client.subscribe(sync_topic.c_str());
      client.subscribe(scan_topic.c_str());
      client.subscribe(update_topic.c_str());

    // Publish discovery configuration first
      publishDiscovery();
      
      // Publish initial online status
      publishStatus();
      last_status_publish = millis();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds...");
      delay(5000);
    }
  }
}

// Main background loop handler to maintain connections
void publishStatus() {
    JsonDocument doc;

    doc["state"] = "online";
    doc["text"] = current_text;

    JsonArray moduleArray = doc["modules"].to<JsonArray>();

    const auto& modules = splitflap.getModules();

    for (size_t i = 0; i < modules.size(); i++) {
        const Module& mod = modules[i];

        JsonObject module = moduleArray.add<JsonObject>();

        module["id"] = mod.id;
        module["state"] = mod.online ? "online" : "offline";
        module["online"] = mod.online;

        module["total_steps"] = mod.stepsPerRev;
        module["status"] = mod.status;
        module["home_offset"] = mod.homeOffset;
        module["auto_homing"] = mod.autoHome;

        if (mod.flapList.length() > 0) {
            module["flap_list"] = mod.flapList;
        }
        if (mod.positionMap.length() > 0) {
            module["map_of_positions"] = mod.positionMap;
        }

        if (mod.flap != '\0') {
            char flap[2] = { mod.flap, '\0' };
            module["flap"] = flap;
        } else if (i < current_text.length()) {
            char flap[2] = { current_text.charAt(i), '\0' };
            module["flap"] = flap;
        } else {
            module["flap"] = "";
        }
    }

    doc["modules_count"] = modules.size();

    String payload;
    serializeJson(doc, payload);

    // A full map for every module can be much larger than the default MQTT
    // buffer. Grow it once instead of silently dropping the publish.
    const size_t needed = payload.length() + 1;
    if (needed > 65535) {
        Serial.println("Status payload is too large for MQTT, not publishing");
        return;
    }
    if (needed > mqtt_buffer_size) {
        if (client.setBufferSize(static_cast<uint16_t>(needed))) {
            mqtt_buffer_size = needed;
        }
    }

    client.publish(status_topic.c_str(), payload.c_str(), true);
}


void handleMqtt() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection lost. Reconnecting...");
        connectWifi();
    }

    if (!client.connected()) {
        connectMqtt();
    }

    client.loop();
}

// Kept for the sync/scan callbacks. There is no separate test payload anymore:
// both paths publish the same real state to avoid phantom modules.