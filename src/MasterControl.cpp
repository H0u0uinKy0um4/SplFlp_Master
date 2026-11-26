#include "MasterControl.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include "BusTrace.h"
#include "Config.h"
#include "ContractSnapshot.h"
#include "ModuleRuntime.h"
#include "ota/Esp32FirmwareUpdater.h"
#include "ota/MasterFirmwareSession.h"
#include "ota/ModuleFirmwareSession.h"

MasterControl masterControl;

namespace {

// A heartbeat keeps Home Assistant from marking the display offline while
// it is idle. Scan progress is throttled separately.
constexpr unsigned long STATUS_PUBLISH_INTERVAL_MS = 30000;
constexpr unsigned long SCAN_PUBLISH_INTERVAL_MS = 1000;

}  // namespace

void MasterControl::begin() {
    // All session policy stays in the firmware sessions; these are the only
    // hooks they need from the rest of the master.
    firmwareSession.setPump([]() { masterControl.pump(); });
    firmwareSession.setNotifier([]() { masterControl.publishStatus(); });
    firmwareSession.setRecovery(
        [](uint8_t id) { moduleRuntime.recoverAfterFirmware(id); });

    masterFirmwareSession.setUpdater(&esp32FirmwareUpdater);
    // finish() only verifies the digest and arms the boot marker; re-entering
    // client.loop() from the MQTT callback is unsafe and unnecessary here.
    masterFirmwareSession.setPump(nullptr);
    masterFirmwareSession.setNotifier([]() { masterControl.publishStatus(); });
    masterFirmwareSession.setReboot([]() { ESP.restart(); });
    masterFirmwareSession.setLogger([](const String& line) {
        busTraceLine(BusSource::Master, "[MasterOTA] " + line);
    });
}

bool MasterControl::busAvailable(const char* commandName) {
    // Collection is non-blocking and must not stop display traffic. Only a
    // master image being written to the OTA slot owns the device exclusively.
    if (!firmwareSession.active() && !masterFirmwareSession.applying()) {
        return true;
    }
    busTraceLine(BusSource::Master,
                 String("[OTA] flash session in progress, ignoring ") +
                     commandName);
    return false;
}

void MasterControl::onCommand(const String& topic, const String& message) {
    if (topic == masterTopics::DISPLAY_TOPIC) {
        handleDisplay(message);
    } else if (topic == masterTopics::SYNC_TOPIC) {
        busTraceLine(BusSource::Master, "Sync requested from Home Assistant");
        publishStatus();
    } else if (topic == masterTopics::SCAN_TOPIC) {
        if (!busAvailable("scan request")) return;
        busTraceLine(BusSource::Master, "Bus scan requested from Home Assistant");
        moduleRuntime.startScan(ID_BEGIN, ID_END);
        lastScanPublish = millis();
        publishStatus();
    } else if (topic == masterTopics::UPDATE_TOPIC) {
        if (!busAvailable("refresh request")) return;
        busTraceLine(BusSource::Master, "Module update requested from Home Assistant");
        if (!moduleRuntime.isScanning()) {
            moduleRuntime.updateModules();
        } else {
            busTraceLine(BusSource::Master, "Scan in progress, skipping refresh");
        }
        publishStatus();
    } else if (topic == masterTopics::CALIBRATE_TOPIC) {
        handleCalibrate(message);
    } else if (topic == masterTopics::FIRMWARE_TOPIC) {
        JsonDocument doc;
        if (deserializeJson(doc, message)) {
            busTraceLine(BusSource::Master, "Failed to parse firmware command");
            return;
        }
        if (firmwareSession.handleAction(doc.as<JsonObject>(),
                                         moduleRuntime.isScanning())) {
            publishStatus();
        }
    } else if (topic == masterTopics::MASTER_FIRMWARE_TOPIC) {
        JsonDocument doc;
        if (deserializeJson(doc, message)) {
            busTraceLine(BusSource::Master,
                         "Failed to parse master firmware command");
            return;
        }
        // The session publishes its own state transitions, including progress
        // milestones and rejections.
        masterFirmwareSession.handleAction(doc.as<JsonObject>());
    }
}

void MasterControl::handleDisplay(const String& message) {
    if (!busAvailable("display command")) return;

    JsonDocument doc;
    if (deserializeJson(doc, message)) return;

    JsonArray moduleArray = doc["modules"].as<JsonArray>();
    if (!moduleArray.isNull()) {
        String assembled;
        for (JsonObject module : moduleArray) {
            const int id = module["id"] | -1;
            const char* value = module["char"] | "";
            char flap = value[0];

            // Module ids are single bus bytes (1..255). Reject client mistakes
            // instead of addressing another module.
            if (id <= 0 || id > 255 || flap == '\0') continue;
            if (flap == '\r' || flap == '\n') flap = ' ';

            moduleRuntime.moveToFlap(static_cast<uint16_t>(id), flap);
            assembled += flap;
        }
        currentText = assembled;
        busTraceLine(BusSource::Master,
                     "Received per-module display: " + currentText);
        publishStatus();
        return;
    }

    // Backwards compatibility for clients that still send a string.
    if (doc["text"].is<const char*>()) {
        currentText = doc["text"].as<String>();
        busTraceLine(BusSource::Master, "Received display text: " + currentText);
        publishStatus();
    }
}

void MasterControl::handleCalibrate(const String& message) {
    if (!busAvailable("calibrate command")) return;

    JsonDocument doc;
    if (deserializeJson(doc, message)) {
        busTraceLine(BusSource::Master, "Failed to parse calibrate command");
        return;
    }
    const int moduleId = doc["module"] | doc["module_id"] | -1;
    const char* action = doc["action"] | "";
    if (moduleId <= 0 || moduleId > 255) {
        busTraceLine(BusSource::Master, "Invalid module id in calibrate command");
        return;
    }
    String act = String(action);
    act.toLowerCase();

    // Moves are blocking on the slave. Module Runtime owns the command, the
    // move window and the readback; `moved` only controls publishing here.
    bool moved = false;
    bool refreshed = false;

    if (act == "goto") {
        if (doc["index"].is<int>()) {
            moved = moduleRuntime.moveToIndex(moduleId, doc["index"].as<int>());
        } else if (doc["flap"].is<const char*>()) {
            char flap = doc["flap"].as<const char*>()[0];
            if (flap == '\r' || flap == '\n') flap = ' ';
            moved = moduleRuntime.moveToFlap(moduleId, flap);
        }
    } else if (act == "home") {
        moved = moduleRuntime.moveHome(moduleId);
    } else if (act == "step") {
        if (doc["steps"].is<int>()) {
            moved = moduleRuntime.moveSteps(moduleId, doc["steps"].as<int>());
        }
    } else if (act == "record") {
        if (doc["index"].is<int>()) {
            moduleRuntime.recordIndex(moduleId, doc["index"].as<int>());
        }
    } else if (act == "apply") {
        if (doc["home_offset"].is<int>() || doc["homeOffset"].is<int>()) {
            const int hoff = doc["home_offset"].is<int>()
                                 ? doc["home_offset"].as<int>()
                                 : doc["homeOffset"].as<int>();
            moduleRuntime.applyHomeOffset(moduleId, hoff);
        }
        if (doc["clear_map"].is<bool>() && doc["clear_map"].as<bool>()) {
            moduleRuntime.clearPositionMap(moduleId);
        }
        if (doc["positions"].is<JsonObject>()) {
            JsonObject pos = doc["positions"].as<JsonObject>();
            for (JsonPair p : pos) {
                moduleRuntime.writePosition(
                    moduleId, String(p.key().c_str()).toInt(),
                    p.value().as<int>());
            }
        }
        moved = moduleRuntime.moveHome(moduleId);
    } else if (act == "clear_map" || act == "clearmap") {
        moduleRuntime.clearPositionMap(moduleId);
    } else if (act == "drum_write" || act == "drumwrite" ||
               act == "write_drum") {
        const String drum = doc["drum"] | "";
        refreshed = moduleRuntime.writeDrum(moduleId, drum);
        if (!refreshed) {
            busTraceLine(BusSource::Master,
                         "Invalid drum length: " + String(drum.length()) +
                             " (expected " + String(DRUM_SIZE) + ")");
        }
    } else if (act == "refresh") {
        moduleRuntime.updateModules();
        publishStatus();
        return;
    } else if (act == "get_pos" || act == "getpos" ||
               act == "get_position") {
        refreshed = moduleRuntime.refreshModule(
            static_cast<uint16_t>(moduleId));
    }

    if (moved || refreshed) {
        publishStatus();
    } else {
        moduleRuntime.updateModules();
        publishStatus();
    }
}

void MasterControl::endCommand() {
    inCommand_ = false;
    if (publishPending_) {
        publishPending_ = false;
        publishStatus();
    }
}

void MasterControl::publishStatus() {
    // Publishing from inside PubSubClient's callback fails and can drop the
    // connection. Coalesce every state change in one command into one publish
    // after the callback returns.
    if (inCommand_) {
        publishPending_ = true;
        return;
    }

    JsonDocument doc;
    doc["state"] = "online";
    doc["text"] = currentText;
    applyMasterContract(doc.as<JsonObject>());
    applyMasterIp(doc.as<JsonObject>(), localIp_.c_str());

    MasterFirmwareStatusSnapshot masterFirmware;
    masterFirmware.state = masterFirmwareSession.stateString();
    masterFirmware.progress = masterFirmwareSession.progress();
    masterFirmware.received = masterFirmwareSession.received();
    masterFirmware.size = masterFirmwareSession.size();
    masterFirmware.targetVersion = masterFirmwareSession.targetVersion();
    masterFirmware.error = masterFirmwareSession.error();
    applyMasterFirmwareStatus(doc.as<JsonObject>(), masterFirmware);

    doc["scanning"] = moduleRuntime.isScanning();
    const int scanProgress = moduleRuntime.scanProgress();
    if (scanProgress >= 0) doc["scan_progress"] = scanProgress;

    JsonArray moduleArray = doc["modules"].to<JsonArray>();
    const auto& modules = moduleRuntime.getModules();
    for (const auto& mod : modules) {
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
        module["current_index"] = mod.currentFlapIndex;
        module["busy"] = mod.busyUntilMs != 0 &&
                         (long)(millis() - mod.busyUntilMs) < 0;
        module["position_known"] = mod.online && mod.positionKnown;

        if (mod.flapList.length() > 0) module["flap_list"] = mod.flapList;
        if (mod.positionMap.length() > 0)
            module["map_of_positions"] = mod.positionMap;

        ModuleVersionSnapshot version;
        version.version = mod.fwVersion;
        version.busCompat = mod.fwBusCompat;
        applyModuleVersion(module, version);

        if (mod.flap != '\0') {
            char flap[2] = {mod.flap, '\0'};
            module["flap"] = flap;
        } else {
            module["flap"] = "";
        }
    }
    doc["modules_count"] = modules.size();

    FirmwareStatusSnapshot session;
    session.state = firmwareSession.stateString();
    session.module = firmwareSession.targetModule();
    session.bootloaderVersion = firmwareSession.bootloaderVersion();
    session.collecting = firmwareSession.collecting();
    session.received = firmwareSession.imageSize();
    session.progress = firmwareSession.progress();
    session.error = firmwareSession.error();
    applyFirmwareStatus(doc.as<JsonObject>(), session);

    String payload;
    serializeJson(doc, payload);
    if (publisher_) {
        publisher_(String(masterTopics::STATUS), payload, true);
    }

    // Stamp the heartbeat even when the publish failed; a permanent failure
    // must not turn this into a tight retry loop.
    lastStatusPublish = millis();
}

void MasterControl::tick() {
    if (moduleRuntime.isScanning()) {
        moduleRuntime.scanStep();
        if (!moduleRuntime.isScanning()) {
            lastScanPublish = 0;
            publishStatus();
        } else if (millis() - lastScanPublish >= SCAN_PUBLISH_INTERVAL_MS) {
            lastScanPublish = millis();
            publishStatus();
        }
        return;
    }

    if (moduleRuntime.tick(millis())) {
        publishStatus();
        return;
    }

    if (millis() - lastStatusPublish >= STATUS_PUBLISH_INTERVAL_MS) {
        publishStatus();
    }
}
