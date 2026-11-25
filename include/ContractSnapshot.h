#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>

// The compatibility/version contract shared by master, slave modules and
// Home Assistant. All parsing, composition and legacy aliases live here so a
// Status Payload cannot disagree with itself.

struct MasterContractSnapshot {
    const char* version = "";
    uint8_t busCompat = 0;
    uint8_t mqttCompat = 0;
};

struct ModuleStateSnapshot {
    uint32_t currentPos = 0;
    bool positionKnown = false;
    uint8_t busCompat = 0;
    String fwVersion;
};

struct ModuleVersionSnapshot {
    String version;
    uint8_t busCompat = 0;
};

struct FirmwareStatusSnapshot {
    const char* state = "idle";
    uint8_t module = 0;
    uint8_t bootloaderVersion = 0;
    bool collecting = false;
    uint16_t received = 0;
    int progress = -1;
    const char* error = "";
};

MasterContractSnapshot masterContractSnapshot();

// Parse an m<id>s reply. Older slaves stop after position; newer ones append
// bus compat and application version. The MQTT generation is never on the
// RS485 bus.
bool parseModuleState(uint16_t id, const String& reply, ModuleStateSnapshot& out);

// Render the master identity and its legacy `compat` alias.
void applyMasterContract(JsonObject status);

// Render module version fields, including fw_compat/fw_compatible aliases.
void applyModuleVersion(JsonObject module, const ModuleVersionSnapshot& version);

// Render the module firmware session object used by the panel.
void applyFirmwareStatus(JsonObject status, const FirmwareStatusSnapshot& session);
