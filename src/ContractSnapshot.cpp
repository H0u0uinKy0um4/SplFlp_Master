#include "ContractSnapshot.h"
#include "Version.h"

MasterContractSnapshot masterContractSnapshot() {
    MasterContractSnapshot snapshot;
    snapshot.version = MASTER_VERSION;
    snapshot.busCompat = MASTER_BUS_COMPAT;
    snapshot.mqttCompat = MASTER_MQTT_COMPAT;
    return snapshot;
}

bool parseModuleState(uint16_t id, const String& reply, ModuleStateSnapshot& out) {
    const String prefix = "m" + String(id) + "s:";
    if (!reply.startsWith(prefix)) return false;

    // m<id>s:<pos>:<busy>:<position_known>[:<bus_compat>:<fw_version>]
    const int p1 = reply.indexOf(':');
    const int p2 = p1 >= 0 ? reply.indexOf(':', p1 + 1) : -1;
    const int p3 = p2 >= 0 ? reply.indexOf(':', p2 + 1) : -1;
    const int p4 = p3 >= 0 ? reply.indexOf(':', p3 + 1) : -1;
    const int p5 = p4 >= 0 ? reply.indexOf(':', p4 + 1) : -1;
    if (p1 <= 0 || p2 <= p1) return false;

    out.currentPos = (uint32_t)reply.substring(p1 + 1, p2).toInt();
    // Old slaves stop after pos and have no hall reference flag: treat that
    // as position known, as the master always did.
    out.positionKnown =
        p3 > p2 ? reply.substring(p3 + 1, p4 > p3 ? p4 : reply.length()).toInt() != 0
                : true;

    if (p4 > p3) {
        String compat = reply.substring(p4 + 1, p5 > p4 ? p5 : reply.length());
        compat.trim();
        if (compat.length() > 0) out.busCompat = (uint8_t)compat.toInt();
    }
    if (p5 > p4) {
        String version = reply.substring(p5 + 1);
        version.trim();
        out.fwVersion = version;
    }
    return true;
}

void applyMasterContract(JsonObject status) {
    const MasterContractSnapshot snapshot = masterContractSnapshot();
    status["version"] = snapshot.version;
    status["bus_compat"] = snapshot.busCompat;
    status["mqtt_compat"] = snapshot.mqttCompat;
    // Older Home Assistant builds only know `compat`.
    status["compat"] = snapshot.mqttCompat;
}

void applyModuleVersion(JsonObject module, const ModuleVersionSnapshot& version) {
    if (version.version.length() > 0) {
        module["fw_version"] = version.version;
    }
    if (version.busCompat > 0) {
        module["fw_bus_compat"] = version.busCompat;
        module["fw_bus_compatible"] = version.busCompat == SLAVE_BUS_COMPAT;
        // Legacy aliases kept for older Home Assistant builds.
        module["fw_compat"] = version.busCompat;
        module["fw_compatible"] = version.busCompat == SLAVE_BUS_COMPAT;
    }
}

void applyFirmwareStatus(JsonObject status, const FirmwareStatusSnapshot& session) {
    // "idle" unless an image collection or flash session is in progress — or
    // has just finished, so the outcome stays readable until the next begin.
    JsonObject ota = status["ota"].to<JsonObject>();
    ota["state"] = session.state;
    if (session.module != 0) {
        ota["module"] = session.module;
    }
    if (session.bootloaderVersion != 0) {
        ota["bootloader_version"] = session.bootloaderVersion;
    }
    if (session.collecting) {
        ota["received"] = session.received;
    }
    if (session.progress >= 0) {
        ota["progress"] = session.progress;
    }
    if (session.error != nullptr && session.error[0] != '\0') {
        ota["error"] = session.error;
    }
}
