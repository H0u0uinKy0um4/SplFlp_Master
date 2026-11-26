#include "network/WifiMqttHandler.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "BusTrace.h"
#include "MasterControl.h"
#include "Version.h"
#include "secrets.h"

namespace {

const char* wifiSsid = WIFI_SSID;
const char* wifiPassword = WIFI_PASSWORD;
const char* mqttPassword = MQTT_PASSWORD;
const char* mqttHost = MQTT_HOST;
const char* mqttUser = MQTT_USER;
const int mqttPort = 1883;
const char* const MQTT_CLIENT_ID = "SplitFlapMaster";

WiFiClient wifiClient;
PubSubClient client(wifiClient);

// PubSubClient needs room for the whole fixed header, topic-length bytes,
// topic and payload. Master firmware chunks are base64-encoded in the client,
// so reserve enough for a ~16 KB chunk before loop() parses it: growing the
// buffer inside the callback would already be too late.
constexpr size_t MQTT_BUFFER_SIZE = 24576;
size_t mqttBufferSize = MQTT_BUFFER_SIZE;

constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;

unsigned long wifiRetryMs = 0;
unsigned long mqttRetryMs = 0;
bool wifiAtFullPower = false;

// The offline will is retained too, so it carries the same additive fields as
// the live Status Payload. The version comes from Version.h, never a literal.
String offlineWillPayload() {
    return String("{\"state\":\"offline\",\"modules\":[],\"modules_count\":0,")
           + "\"ip\":\"\",\"master_ota\":{\"state\":\"idle\",\"progress\":0,"
           + "\"received\":0,\"size\":0,\"version\":\"" + MASTER_VERSION
           + "\",\"target_version\":\"\",\"error\":\"\"}}";
}

// Firmware-side discovery duplicates the entities created by the Home
// Assistant integration, so it is disabled by default.
#ifndef SPLFLAP_ENABLE_MQTT_DISCOVERY
#define SPLFLAP_ENABLE_MQTT_DISCOVERY 0
#endif

constexpr char DISCOVERY_TOPIC[] =
    "homeassistant/sensor/splflp_master_status/config";
// Older firmware published a second config with the same unique_id here.
constexpr char LEGACY_DEVICE_DISCOVERY_TOPIC[] =
    "homeassistant/device/splflp_master/config";

void publishDiscovery() {
    client.publish(LEGACY_DEVICE_DISCOVERY_TOPIC, "", true);

#if SPLFLAP_ENABLE_MQTT_DISCOVERY
    JsonDocument doc;
    doc["name"] = "SplitFlap Status";
    doc["state_topic"] = masterTopics::STATUS;
    doc["unique_id"] = "splflp_master_status";
    doc["value_template"] = "{{ value_json.state }}";

    JsonObject device = doc["device"].to<JsonObject>();
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
    client.publish(DISCOVERY_TOPIC, "", true);
#endif
}

bool publishMqtt(const String& topic, const String& payload, bool retained) {
    const size_t needed =
        payload.length() + topic.length() + MQTT_MAX_HEADER_SIZE + 2;
    if (needed > 65535) {
        busTraceLine(BusSource::Master,
                     "Status payload is too large for MQTT, not publishing");
        return false;
    }
    if (needed > mqttBufferSize) {
        if (!client.setBufferSize(static_cast<uint16_t>(needed))) {
            busTraceLine(BusSource::Master,
                         "Failed to grow MQTT buffer, not publishing");
            return false;
        }
        mqttBufferSize = needed;
    }
    if (!client.publish(topic.c_str(), payload.c_str(), retained)) {
        busTraceLine(BusSource::Master, "MQTT publish of the status failed");
        return false;
    }
    return true;
}

// Start WiFi without waiting. handleMqtt() retries in the background so a
// router that boots after the display is picked up without a reboot.
void beginWifiImpl() {
    Serial.println("Connecting to WiFi...");
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(wifiSsid, wifiPassword);
}

bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiRetryMs = 0;
        if (!wifiAtFullPower) {
            wifiAtFullPower = true;
            WiFi.setTxPower(WIFI_POWER_15dBm);
            Serial.print("WiFi connected! IP address: ");
            Serial.println(WiFi.localIP());
            // Additive status field: ArduinoOTA recovery needs the address.
            masterControl.setLocalIp(WiFi.localIP().toString());
            masterControl.publishStatus();
        }
        return true;
    }

    wifiAtFullPower = false;
    const unsigned long now = millis();
    if (wifiRetryMs != 0 && now - wifiRetryMs < WIFI_RETRY_INTERVAL_MS) {
        return false;
    }
    wifiRetryMs = now == 0 ? 1 : now;
    Serial.println("WiFi connection lost, restarting association attempt");
    WiFi.disconnect();
    WiFi.begin(wifiSsid, wifiPassword);
    return false;
}

bool ensureMqtt() {
    if (client.connected()) {
        mqttRetryMs = 0;
        return true;
    }

    const unsigned long now = millis();
    if (mqttRetryMs != 0 && now - mqttRetryMs < MQTT_RETRY_INTERVAL_MS) {
        return false;
    }
    mqttRetryMs = now == 0 ? 1 : now;

    Serial.print("Attempting MQTT connection...");
    const String offlineWill = offlineWillPayload();
    if (!client.connect(MQTT_CLIENT_ID, mqttUser, mqttPassword,
                        masterTopics::STATUS, 1, true,
                        offlineWill.c_str())) {
        Serial.printf("failed, rc=%d (retrying in %lus)\n", client.state(),
                      MQTT_RETRY_INTERVAL_MS / 1000UL);
        return false;
    }
    Serial.println("connected!");

    client.subscribe(masterTopics::DISPLAY_TOPIC);
    client.subscribe(masterTopics::SYNC_TOPIC);
    client.subscribe(masterTopics::SCAN_TOPIC);
    client.subscribe(masterTopics::UPDATE_TOPIC);
    client.subscribe(masterTopics::CALIBRATE_TOPIC);
    client.subscribe(masterTopics::FIRMWARE_TOPIC);
    client.subscribe(masterTopics::MASTER_FIRMWARE_TOPIC);

    publishDiscovery();
    masterControl.publishStatus();
    return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

    // Tag the whole request as HA-originated on the web console, then hand
    // interpretation to the deep control module.
    busTraceLine(BusSource::Ha, formatCommandTrace(String(topic), message));
    masterControl.beginCommand();
    masterControl.onCommand(String(topic), message);
}

}  // namespace

void beginWifi() {
    beginWifiImpl();
}

void setupMqtt() {
    client.setBufferSize(static_cast<uint16_t>(mqttBufferSize));
    client.setServer(mqttHost, mqttPort);
    client.setCallback(mqttCallback);

    masterControl.setPublisher(publishMqtt);
    masterControl.setPump([]() { client.loop(); });
    masterControl.begin();

    // Command handling stays inside the callback, but status publishes are
    // deferred until client.loop() returns (MasterControl::beginCommand/endCommand).
    Serial.println("setUp Mqtt");
}

void handleMqtt() {
    if (!ensureWifi()) return;
    if (!ensureMqtt()) return;

    client.loop();
    // Publish after the callback has returned to PubSubClient. A pending flag
    // survives a dropped connection and is flushed on the next loop pass.
    masterControl.endCommand();
    if (!client.connected()) return;

    // Commands are served by the callback. Time-driven work (scan slices,
    // settled-move readback and heartbeat) lives in MasterControl.
    masterControl.tick();
}
