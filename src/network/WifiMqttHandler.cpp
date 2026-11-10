#include "network/WifiMqttHandler.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "web/WebSerialConsole.h"
#include "Display.h"
#include "RS485.h"


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
String calibrate_topic = String(topic_prefix) + "/command/calibrate";

WiFiClient wifiClient;
PubSubClient client(wifiClient);
String current_text = "";
unsigned long last_status_publish = 0;

// millis() deadline for a refresh that was postponed because the slave's move
// is blocking. 0 means no refresh is pending.
unsigned long refresh_due_ms = 0;

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

// Retry pacing for the non-blocking connection management in handleMqtt().
// An outage must never freeze the loop: the web console and OTA are the tools
// used to diagnose a network problem, so they have to keep running while
// WiFi or the broker is unreachable. Retry deadlines of 0 mean "not pending".
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;

// Progress of an incremental bus scan is published at most this often.
constexpr unsigned long SCAN_PUBLISH_INTERVAL_MS = 1000;

unsigned long wifi_retry_ms = 0;
unsigned long mqtt_retry_ms = 0;
// The radio associates at reduced TX power and is raised afterwards (some
// boards fail to associate at full power).
bool wifi_at_full_power = false;
unsigned long last_scan_publish = 0;

// Start the WiFi connection without waiting for it. handleMqtt() keeps
// retrying in the background, so a router that comes up after the display
// (e.g. a power cut where the router boots slower) is picked up without a
// reboot.
void beginWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(wifi_ssid, wifi_password);
}

// True while WiFi is connected; restarts the association attempt at most
// every WIFI_RETRY_INTERVAL_MS. WiFi.begin() is asynchronous, so this never
// blocks.
bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifi_retry_ms = 0;
    if (!wifi_at_full_power) {
      wifi_at_full_power = true;
      WiFi.setTxPower(WIFI_POWER_15dBm);
      Serial.print("WiFi connected! IP address: ");
      Serial.println(WiFi.localIP());
    }
    return true;
  }

  wifi_at_full_power = false;
  const unsigned long now = millis();
  if (wifi_retry_ms != 0 && now - wifi_retry_ms < WIFI_RETRY_INTERVAL_MS) {
    return false;
  }
  wifi_retry_ms = now == 0 ? 1 : now;
  Serial.println("WiFi connection lost, restarting association attempt");
  WiFi.disconnect();
  WiFi.begin(wifi_ssid, wifi_password);
  return false;
}

// True when the MQTT client is connected; attempts a single connect at most
// every MQTT_RETRY_INTERVAL_MS. The TCP connect inside client.connect() can
// still block for lwIP's SYN timeout when the broker IP is unroutable, but
// the retry pacing bounds how often that can happen.
bool ensureMqtt() {
  if (client.connected()) {
    mqtt_retry_ms = 0;
    return true;
  }

  const unsigned long now = millis();
  if (mqtt_retry_ms != 0 && now - mqtt_retry_ms < MQTT_RETRY_INTERVAL_MS) {
    return false;
  }
  mqtt_retry_ms = now == 0 ? 1 : now;

  Serial.print("Attempting MQTT connection...");
  if (client.connect(MQTT_CLIENT_ID, mqtt_user, mqtt_password,
                      status_topic.c_str(), 1, true, OFFLINE_WILL_PAYLOAD)) {
    Serial.println("connected!");

    // Resubscribe to command topics upon successful connection
    client.subscribe(display_topic.c_str());
    client.subscribe(sync_topic.c_str());
    client.subscribe(scan_topic.c_str());
    client.subscribe(update_topic.c_str());
    client.subscribe(calibrate_topic.c_str());

    // Publish discovery configuration first, then the initial online status
    // (publishStatus also stamps the heartbeat timer).
    publishDiscovery();
    publishStatus();
    return true;
  }

  Serial.printf("failed, rc=%d (retrying in %lus)\n", client.state(),
                 MQTT_RETRY_INTERVAL_MS / 1000UL);
  return false;
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

                  // The module id is a single byte on the bus (1..255);
                  // anything else can only be a client mistake.
                  if (id <= 0 || id > 255 || flap == '\0') {
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

        // Incremental scan: the loop keeps running (MQTT, web console, OTA)
        // while the bus is walked. Progress rides along on the status
        // payload; handleMqtt() drives the sweep one slice at a time.
        splitflap.startScan(ID_BEGIN, ID_END);

        publishStatus();
        last_scan_publish = millis();
    }

    // UPDATE COMMAND
    else if (incomingTopic == update_topic) {

        Serial.println("Module update requested from Home Assistant!");

        // Refresh only the modules that are already known. Unlike a scan this
        // does not clear the module list or walk every possible ID, so it is
        // fast and safe to call frequently.
        // During an incremental scan every module is being probed anyway;
        // walking them again here would block the loop for seconds.
        if (!splitflap.isScanning()) {
            splitflap.updateModules();
        } else {
            Serial.println("Scan in progress, skipping explicit refresh");
        }

        publishStatus();
    }
    // CALIBRATE COMMAND
    else if (incomingTopic == calibrate_topic) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, message);
        if (error) {
            Serial.println("Failed to parse calibrate command");
            return;
        }
        int moduleId = doc["module"] | doc["module_id"] | -1;
        const char* action = doc["action"] | "";
        if (moduleId <= 0 || moduleId > 255) {
            Serial.println("Invalid module id in calibrate command");
            return;
        }
        String act = String(action);
        act.toLowerCase();

        // The slave performs a move as a blocking loop, so it cannot answer a
        // query until it is done. Track whether this command starts a move and
        // postpone the refresh in that case instead of marking the module
        // offline while it is still turning.
        bool moved = false;
        bool refreshed = false;

        if (act == "goto") {
            if (doc["index"].is<int>()) {
                int index = doc["index"].as<int>();
                String cmd = "m" + String(moduleId) + "+" + String(index);
                rs485Send(cmd);
                splitflap.markMovingIndex(moduleId, index);
                moved = true;
            } else if (doc["flap"].is<const char*>()) {
                char flap = doc["flap"].as<const char*>()[0];
                if (flap == '\r' || flap == '\n') flap = ' ';
                String cmd = "m" + String(moduleId) + "-" + String(flap);
                rs485Send(cmd);
                splitflap.markMovingFlap(moduleId, flap);
                moved = true;
            }
        } else if (act == "home") {
            String cmd = "m" + String(moduleId) + "h";
            rs485Send(cmd);
            splitflap.markMovingHome(moduleId);
            moved = true;
        } else if (act == "step") {
            if (doc["steps"].is<int>()) {
                int steps = doc["steps"].as<int>();
                String cmd = "m" + String(moduleId) + "g" + String(steps);
                rs485Send(cmd);
                splitflap.markMovingPos(moduleId, steps);
                moved = true;
            }
        } else if (act == "record") {
            if (doc["index"].is<int>()) {
                // The slave records its current position and, for index 0,
                // redefines homeOffset. Do not send the old compatibility
                // write: it overwrote flap 0 with position 0.
                String cmd = "m" + String(moduleId) + "r" + String(doc["index"].as<int>());
                rs485Send(cmd);
            }
        } else if (act == "apply") {
            if (doc["home_offset"].is<int>() || doc["homeOffset"].is<int>()) {
                int hoff = doc["home_offset"].is<int>() ? doc["home_offset"].as<int>() : doc["homeOffset"].as<int>();
                String cmd = "m" + String(moduleId) + "o" + String(hoff);
                rs485Send(cmd);
                delay(50);
            }
            if (doc["clear_map"].is<bool>() && doc["clear_map"].as<bool>()) {
                String cmd = "m" + String(moduleId) + "e*";
                rs485Send(cmd);
                delay(50);
            }
            if (doc["positions"].is<JsonObject>()) {
                JsonObject pos = doc["positions"].as<JsonObject>();
                for (JsonPair p : pos) {
                    String cmd = "m" + String(moduleId) + "w" + String(p.key().c_str()) + ":" + String(p.value().as<int>());
                    rs485Send(cmd);
                    delay(30);
                }
            }
            String homeCmd = "m" + String(moduleId) + "h";
            rs485Send(homeCmd);
            splitflap.markMovingHome(moduleId);
            moved = true;
        } else if (act == "clear_map" || act == "clearmap") {
            String cmd = "m" + String(moduleId) + "e*";
            rs485Send(cmd);
        } else if (act == "drum_write" || act == "drumwrite" || act == "write_drum") {
            String drum = doc["drum"] | "";
            bool valid = drum.length() == DRUM_SIZE;
            // A newline would terminate the RS485 command line early.
            for (int i = 0; valid && i < DRUM_SIZE; i++) {
                if (drum[i] == '\r' || drum[i] == '\n') valid = false;
            }
            if (valid) {
                for (int i = 0; i < DRUM_SIZE; i++) {
                    String cmd = "m" + String(moduleId) + "f" + String(i) + ":" + String(drum[i]);
                    rs485Send(cmd);
                    delay(30);
                }
                // Read the drum back so a failed write is visible in Home
                // Assistant instead of leaving the cached flap list stale.
                delay(100);
                refreshed = splitflap.refreshModule(static_cast<uint16_t>(moduleId));
            } else {
                Serial.printf("Invalid drum length: %d (expected %d)\n", drum.length(), DRUM_SIZE);
            }
        } else if (act == "refresh") {
            splitflap.updateModules();
            publishStatus();
            return;
        } else if (act == "get_pos" || act == "getpos" || act == "get_position") {
            refreshed = splitflap.refreshModule(static_cast<uint16_t>(moduleId));
        }

        if (moved) {
            // Report the requested state now, then refresh once the blocking
            // move has had time to finish.
            publishStatus();
            refresh_due_ms = millis() + MOVE_SETTLE_MS;
        } else {
            if (!refreshed) splitflap.updateModules();
            publishStatus();
        }
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

// Main background loop handler to maintain connections
void publishStatus() {
    JsonDocument doc;

    doc["state"] = "online";
    doc["text"] = current_text;

    // Incremental scan progress. The integration ignores these when no scan
    // is running (scan_progress is only published during one).
    doc["scanning"] = splitflap.isScanning();
    const int scanProgress = splitflap.scanProgress();
    if (scanProgress >= 0) doc["scan_progress"] = scanProgress;

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
        module["current_step"] = mod.currentPos;
        module["current_pos"] = mod.currentPos;
        module["pos"] = mod.currentPos;
        // Drum index derived from the step position when it is known; -1
        // when the position is unknown or cannot be mapped to a flap.
        module["current_index"] = mod.currentFlapIndex;
        // Real per-module move window: true from the moment a movement
        // command is sent until the post-move re-query has refreshed the
        // position. Old firmware always published false here.
        module["busy"] = mod.busyUntilMs != 0 &&
                         (long)(millis() - mod.busyUntilMs) < 0;
        module["position_known"] = mod.online && mod.positionKnown;

        if (mod.flapList.length() > 0) {
            module["flap_list"] = mod.flapList;
        }
        if (mod.positionMap.length() > 0) {
            module["map_of_positions"] = mod.positionMap;
        }

        if (mod.flap != '\0') {
            char flap[2] = { mod.flap, '\0' };
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
    // PubSubClient's publish() requires room for the whole fixed header
    // (MQTT_MAX_HEADER_SIZE), the two topic-length bytes, the topic and the
    // payload; sizing it to the payload alone made ~2 kB status payloads
    // fail silently.
    const size_t needed = payload.length() + status_topic.length() +
                         MQTT_MAX_HEADER_SIZE + 2;
    if (needed > 65535) {
        Serial.println("Status payload is too large for MQTT, not publishing");
    } else if (needed > mqtt_buffer_size) {
        if (client.setBufferSize(static_cast<uint16_t>(needed))) {
            mqtt_buffer_size = needed;
        } else {
            Serial.println("Failed to grow MQTT buffer, not publishing");
        }
    }

    // Publish only when the buffer actually covers the payload; a skipped
    // publish is already logged above.
    if (needed <= 65535 && needed <= mqtt_buffer_size) {
        if (!client.publish(status_topic.c_str(), payload.c_str(), true)) {
            Serial.println("MQTT publish of the status payload failed");
        }
    }

    // Update the heartbeat timestamp even when nothing was published: a
    // permanent failure (payload too large, buffer allocation failed) must
    // not turn the heartbeat into a tight retry loop that spams the log on
    // every loop pass. Failed publishes retry at the normal heartbeat
    // interval instead.
    last_status_publish = millis();
}


void handleMqtt() {
    // WiFi and the broker are re-established without blocking: an outage
    // must not freeze the web console, OTA or the display command path.
    if (!ensureWifi()) return;
    if (!ensureMqtt()) return;

    client.loop();
    // The connection can drop mid-loop; never use a dead client below.
    if (!client.connected()) return;

    // Drive an in-flight bus scan one time slice at a time. Commands keep
    // being served between slices because client.loop() runs every pass.
    if (splitflap.isScanning()) {
        splitflap.scanStep();
        if (!splitflap.isScanning()) {
            // The scan just finished: publish the pruned module list right
            // away instead of waiting for the 30 s heartbeat.
            last_scan_publish = 0;
            publishStatus();
        } else if (millis() - last_scan_publish >= SCAN_PUBLISH_INTERVAL_MS) {
            last_scan_publish = millis();
            publishStatus();
        }
        return;
    }

    const unsigned long now = millis();

    // Finish a refresh that was postponed because a slave move is blocking.
    if (refresh_due_ms != 0 && (long)(now - refresh_due_ms) >= 0) {
        refresh_due_ms = 0;
        splitflap.updateModules();
        publishStatus();
        return;
    }

    // Modules whose estimated move window just expired are re-queried here,
    // one per loop pass, so their true position and flap reach Home
    // Assistant without anyone polling for them. Commands are never delayed
    // by this; only the readback waits out the move.
    if (splitflap.refreshSettledMoves()) {
        publishStatus();
        return;
    }

    // Periodic heartbeat: without it Home Assistant marks the display offline
    // after OFFLINE_TIMEOUT_SECONDS even though nothing is wrong.
    if (now - last_status_publish >= STATUS_PUBLISH_INTERVAL_MS) {
        publishStatus();
    }
}