#include "ModuleRuntime.h"
#include "ContractSnapshot.h"
#include "Rs485Link.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

ModuleRuntime moduleRuntime;

ModuleRuntime::ModuleRuntime() = default;

// ---------------------------------------------------------------------------
// Module record
// ---------------------------------------------------------------------------

Module::Module() = default;

Module::Module(const String& s) {
    const int start = s.indexOf(':');
    if (start < 0) return;
    id = s.substring(1, start).toInt();

    // m<id>:<homeOffset>:<totalSteps>:<autoHome>:<index>=<pos>,...
    int cursor = start + 1;
    auto takeField = [&s, &cursor]() -> String {
        const int colon = s.indexOf(':', cursor);
        const String value =
            colon < 0 ? s.substring(cursor) : s.substring(cursor, colon);
        cursor = colon < 0 ? s.length() + 1 : colon + 1;
        return value;
    };

    homeOffset = takeField().toInt();
    const uint32_t parsedSteps = takeField().toInt();
    if (parsedSteps > 0) stepsPerRev = parsedSteps;
    autoHome = takeField().toInt() != 0;
    status = autoHome ? 1 : 0;

    if (cursor <= static_cast<int>(s.length())) {
        positionMap = s.substring(cursor);
    }
    online = true;
}

Module* ModuleRuntime::getModule(uint16_t id) {
    for (auto& mod : modules) {
        if (mod.id == id) return &mod;
    }
    return nullptr;
}

const etl::vector<Module, 256>& ModuleRuntime::getModules() const {
    return modules;
}

void ModuleRuntime::sortModules() {
    std::sort(modules.begin(), modules.end(),
              [](const Module& a, const Module& b) { return a.id < b.id; });
}

void ModuleRuntime::begin() {
    EEPROM.begin(EEPROM_SIZE);
    loadEepromModules();
    sortModules();
}

// ---------------------------------------------------------------------------
// Display effects
// ---------------------------------------------------------------------------

bool ModuleRuntime::setModuleChar(uint16_t id, char flap) {
    if (id <= 0 || id >= 256 || flap == '\0') return false;

    // A newline or carriage return would terminate the RS485 line early.
    if (flap == '\r' || flap == '\n') flap = ' ';

    Module* mod = getModule(id);

    // The slave silently ignores a character that is not on its drum, so
    // reject it here instead of reporting a character that will never show.
    if (mod && mod->flapList.length() == DRUM_SIZE &&
        mod->flapList.indexOf(flap) < 0) {
        Serial.printf("[ModuleRuntime] Module %u has no flap '%c'\n", id, flap);
        return false;
    }

    String message;
    message += 'm';
    message += String(id);
    message += '-';
    message += flap;
    rs485SendLine(message);

    // The slave queues lines while it turns; only queries wait out the move.
    markMovingFlap(id, flap);
    delay(BUS_RELEASE_MS);

    // The next status payload reports the requested character while the motor
    // is still turning; settled-move readback replaces it with the real flap.
    if (mod) mod->flap = flap;
    return true;
}

void ModuleRuntime::recoverAfterFirmware(uint8_t id) {
    char lastFlap = '\0';
    for (const auto& mod : modules) {
        if (mod.id == id) {
            lastFlap = mod.flap;
            break;
        }
    }

    // The freshly booted module may auto-home first (a blocking move).
    markMovingHome(id);
    if (lastFlap != '\0') setModuleChar(id, lastFlap);
}

// ---------------------------------------------------------------------------
// Incremental bus scan
// ---------------------------------------------------------------------------

void ModuleRuntime::startScan(uint16_t from, uint16_t to) {
    if (from < ID_BEGIN) from = ID_BEGIN;
    if (to > ID_END) to = ID_END;
    if (from > to) return;

    scanning = true;
    scanFrom = from;
    scanTo = to;
    scanNext = from;
    scanProbed = 0;
    scanStartMs = millis();
    memset(scanFoundMask, 0, sizeof(scanFoundMask));

    // One EEPROM commit in finishScan() persists the count and every id.
    EEPROM.write(ADDR_STORED_IDS, 0);

    // The live module list is deliberately not cleared here: display and
    // calibrate commands keep being served while the scan runs, and a module
    // that is mid-move cannot answer a probe. Non-responders are dropped in
    // finishScan() instead.
    Serial.printf("[ModuleRuntime] Scanning ids %u..%u...\n",
                  (unsigned)from, (unsigned)to);
}

bool ModuleRuntime::scanStep() {
    if (!scanning) return false;

    const unsigned long sliceStart = millis();
    while (scanNext <= scanTo && millis() - sliceStart < SCAN_SLICE_MS) {
        // Probe the live record when the id is already known so an active
        // move window is honoured: a turning module cannot answer, but it is
        // definitionally present.
        Module* live = getModule(scanNext);
        Module fresh;
        Module& probe = live ? *live : fresh;
        if (sendQueryAndWait(scanNext, SCAN_TIMEOUT_MS, probe)) {
            if (!live && !modules.full()) modules.push_back(fresh);
            markScanIdFound(scanNext);
            storeEepromId(scanNext);
        }
        scanNext++;
        scanProbed++;
    }

    if (scanNext > scanTo) {
        finishScan();
        return false;
    }
    return true;
}

void ModuleRuntime::finishScan() {
    scanning = false;

    size_t kept = 0;
    for (size_t i = 0; i < modules.size(); i++) {
        if (scanIdFound(modules[i].id)) {
            if (kept != i) modules[kept] = modules[i];
            kept++;
        }
    }
    modules.resize(kept);

    EEPROM.commit();
    sortModules();
    Serial.printf("[ModuleRuntime] Scan finished: %u module(s) in %lu ms\n",
                  (unsigned)modules.size(), millis() - scanStartMs);
}

bool ModuleRuntime::isScanning() const {
    return scanning;
}

int ModuleRuntime::scanProgress() const {
    if (!scanning) return -1;
    const uint16_t total = scanTo - scanFrom + 1;
    if (total == 0) return 100;
    return (int)((unsigned long)scanProbed * 100UL / total);
}

bool ModuleRuntime::scanIdFound(uint16_t id) const {
    if (id > ID_END) return false;
    return (scanFoundMask[id >> 3] & (1 << (id & 7))) != 0;
}

void ModuleRuntime::markScanIdFound(uint16_t id) {
    if (id > ID_END) return;
    scanFoundMask[id >> 3] |= (1 << (id & 7));
}

void ModuleRuntime::storeEepromId(int id) {
    uint8_t count = EEPROM.read(ADDR_STORED_IDS);
    if (count > 254) count = 0;

    // The table stores its count at address 0 and ids at 1..254.
    if (count >= 254) {
        Serial.println("[EEPROM] Module ID table full, not storing");
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        if (EEPROM.read(ADDR_STORED_IDS + 1 + i) == (uint8_t)id) return;
    }

    EEPROM.write(ADDR_STORED_IDS + 1 + count, (uint8_t)id);
    EEPROM.write(ADDR_STORED_IDS, count + 1);
    Serial.printf("[EEPROM] Queued module ID: %d (total: %u)\n", id, count + 1);
}

void ModuleRuntime::loadEepromModules() {
    uint8_t count = EEPROM.read(ADDR_STORED_IDS);
    if (count > 254) count = 0;
    for (uint8_t i = 0; i < count; i++) {
        Module mod;
        mod.id = EEPROM.read(ADDR_STORED_IDS + 1 + i);
        sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod);
        if (!modules.full()) modules.push_back(mod);
    }
}

void ModuleRuntime::updateModules() {
    // Keep silent modules in the list but mark them offline. Modules inside
    // their move window are skipped and keep their cached state.
    for (auto& mod : modules) {
        if (!sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod)) {
            mod.online = false;
        }
    }
}

bool ModuleRuntime::refreshModule(uint16_t id) {
    Module* mod = getModule(id);
    if (mod) return sendQueryAndWait(id, SCAN_TIMEOUT_MS, *mod);

    if (modules.full()) return false;
    Module fresh;
    if (!sendQueryAndWait(id, SCAN_TIMEOUT_MS, fresh)) return false;
    modules.push_back(fresh);
    sortModules();
    return true;
}

// ---------------------------------------------------------------------------
// Move windows and the single time-driven seam
// ---------------------------------------------------------------------------

long ModuleRuntime::flapTargetPos(const Module& mod, int index) const {
    if (mod.stepsPerRev == 0 || index < 0 || index >= DRUM_SIZE) return -1;
    return ((long)mod.homeOffset +
            ((long)index * (long)mod.stepsPerRev + 32) / 64) %
           (long)mod.stepsPerRev;
}

void ModuleRuntime::extendBusy(Module& mod, unsigned long estMs) {
    const unsigned long now = millis();
    // Commands queued behind a running move extend the deadline.
    if (mod.busyUntilMs != 0 && (long)(mod.busyUntilMs - now) > 0) {
        mod.busyUntilMs += estMs;
    } else {
        mod.busyUntilMs = now + estMs;
    }
    mod.busyMisses = 0;
}

void ModuleRuntime::markMovingFlap(uint16_t id, char flap) {
    Module* mod = getModule(id);
    if (!mod) return;

    const int index = mod->flapList.length() == DRUM_SIZE
                          ? mod->flapList.indexOf(flap)
                          : -1;
    const long target = flapTargetPos(*mod, index);
    if (target < 0) {
        extendBusy(*mod, MOVE_SETTLE_MS);
        return;
    }
    markMovingPos(id, target);
}

void ModuleRuntime::markMovingIndex(uint16_t id, int index) {
    Module* mod = getModule(id);
    if (!mod) return;

    const long target = flapTargetPos(*mod, index);
    if (target < 0) {
        extendBusy(*mod, MOVE_SETTLE_MS);
        return;
    }
    markMovingPos(id, target);
}

void ModuleRuntime::markMovingPos(uint16_t id, long pos) {
    Module* mod = getModule(id);
    if (!mod) return;

    if (mod->stepsPerRev == 0 || pos < 0) {
        extendBusy(*mod, MOVE_SETTLE_MS);
        return;
    }

    long dist = (pos - (long)mod->currentPos) % (long)mod->stepsPerRev;
    if (dist < 0) dist += (long)mod->stepsPerRev;
    if (dist > (long)mod->stepsPerRev) dist = (long)mod->stepsPerRev;
    extendBusy(*mod, (unsigned long)dist + MOVE_MARGIN_MS);
}

void ModuleRuntime::markMovingHome(uint16_t id) {
    Module* mod = getModule(id);
    if (!mod) return;

    // Homing leaves the magnet, seeks the entering edge and runs homeOffset
    // steps to flap 0.
    const unsigned long est = mod->stepsPerRev > 0
                                  ? mod->stepsPerRev + mod->homeOffset
                                  : MOVE_SETTLE_MS;
    extendBusy(*mod, est + MOVE_MARGIN_MS);
}

void ModuleRuntime::deferRefresh(unsigned long delayMs) {
    refreshDueMs = millis() + delayMs;
}

bool ModuleRuntime::tick(unsigned long now) {
    if (refreshDueMs != 0 && (long)(now - refreshDueMs) >= 0) {
        refreshDueMs = 0;
        updateModules();
        return true;
    }
    return refreshSettledMoves(now);
}

bool ModuleRuntime::refreshSettledMoves(unsigned long now) {
    for (auto& mod : modules) {
        if (mod.busyUntilMs == 0) continue;
        if ((long)(now - mod.busyUntilMs) < 0) continue;

        mod.busyUntilMs = 0;
        if (sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod)) {
            mod.busyMisses = 0;
        } else if (++mod.busyMisses < 2) {
            // The move outlived its estimate (or the module went silent).
            mod.busyUntilMs = millis() + MOVE_SETTLE_MS;
        } else {
            mod.online = false;
            mod.busyMisses = 0;
        }
        // One module per loop pass keeps the bus, MQTT and console responsive.
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bus queries
// ---------------------------------------------------------------------------

bool ModuleRuntime::sendQueryAndWait(uint16_t id, uint16_t timeoutMs,
                                     Module& mod) {
    // A module inside its move window cannot answer: the slave blocks in its
    // step loop while it turns. Keep the cached state instead of timing out
    // and reporting the module offline.
    if (mod.busyUntilMs != 0 && (long)(millis() - mod.busyUntilMs) < 0) {
        return mod.online;
    }

    String message;
    message += 'm';
    message += String(id);
    message += 'd';
    rs485SendLine(message);

    // Only accept a response carrying the id we asked for.
    const String configPrefix = "m" + String(id) + ":";
    unsigned long startTime = millis();
    bool gotConfig = false;

    // Keep diagnostics this query does not carry so a transient failure cannot
    // wipe the cached drum, version or position.
    const char previousFlap = mod.flap;
    const String previousFlapList = mod.flapList;
    const String previousFwVersion = mod.fwVersion;
    const uint8_t previousFwBusCompat = mod.fwBusCompat;
    const uint32_t previousPos = mod.currentPos;
    const bool previousPositionKnown = mod.positionKnown;

    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        const String response = rs485ReadLine();
        if (response.length() > 0 && response.startsWith(configPrefix)) {
            mod = Module(response);
            if (mod.flap == '\0') mod.flap = previousFlap;
            gotConfig = mod.online;
            break;
        }
        yield();
    }
    if (!gotConfig) return false;

    delay(BUS_RELEASE_MS);

    // The drum query must be read untrimmed: the stock drum starts with a
    // blank flap and trimming would shift every flap index. Its response has
    // no id prefix, so length is the only validation available.
    rs485SendLine("m" + String(id) + "l");

    startTime = millis();
    bool gotDrum = false;
    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        const String drum = rs485ReadLine(false);
        if (drum.length() > 0) {
            if (drum.length() == DRUM_SIZE) {
                mod.flapList = drum;
                gotDrum = true;
            }
            break;
        }
        yield();
    }
    if (!gotDrum) mod.flapList = previousFlapList;

    delay(BUS_RELEASE_MS);

    // State query: m<id>s:<pos>:<busy>:<position_known>[:<compat>:<version>]
    rs485SendLine("m" + String(id) + "s");

    const String statePrefix = "m" + String(id) + "s:";
    startTime = millis();
    bool gotState = false;
    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        const String resp = rs485ReadLine();
        ModuleStateSnapshot parsed;
        if (parseModuleState(id, resp, parsed)) {
            mod.currentPos = parsed.currentPos;
            mod.positionKnown = parsed.positionKnown;
            if (parsed.busCompat > 0) mod.fwBusCompat = parsed.busCompat;
            if (parsed.fwVersion.length() > 0) mod.fwVersion = parsed.fwVersion;
            gotState = true;
        }
        if (resp.length() > 0 && resp.startsWith(statePrefix)) break;
        yield();
    }
    if (!gotState) {
        mod.currentPos = previousPos;
        mod.positionKnown = previousPositionKnown;
        mod.fwVersion = previousFwVersion;
        mod.fwBusCompat = previousFwBusCompat;
    } else {
        if (mod.fwVersion.length() == 0) mod.fwVersion = previousFwVersion;
        if (mod.fwBusCompat == 0) mod.fwBusCompat = previousFwBusCompat;
        // A trusted position gives the real flap, not the requested one.
        deriveActualFlap(mod);
    }

    delay(BUS_RELEASE_MS);
    return mod.online;
}

// ---------------------------------------------------------------------------
// Actual flap readback
// ---------------------------------------------------------------------------

void ModuleRuntime::deriveActualFlap(Module& mod) const {
    mod.currentFlapIndex = -1;
    if (!mod.online || !mod.positionKnown) return;
    if (mod.flapList.length() != DRUM_SIZE || mod.stepsPerRev == 0) return;

    // Position map format: "index=pos,index=pos" (may be empty).
    if (mod.positionMap.length() > 0) {
        const int mapLen = mod.positionMap.length();
        int cursor = 0;
        while (cursor <= mapLen) {
            const int comma = mod.positionMap.indexOf(',', cursor);
            const int end = comma < 0 ? mapLen : comma;
            const int eq = mod.positionMap.indexOf('=', cursor);
            if (eq > cursor && eq < end) {
                const int index = mod.positionMap.substring(cursor, eq).toInt();
                const long pos = mod.positionMap.substring(eq + 1, end).toInt();
                // 65535 is the slave's "entry unset" sentinel.
                if (index >= 0 && index < DRUM_SIZE && pos >= 0 &&
                    pos < (long)mod.stepsPerRev &&
                    pos == (long)mod.currentPos) {
                    mod.currentFlapIndex = index;
                    mod.flap = mod.flapList[index];
                    return;
                }
            }
            if (comma < 0) break;
            cursor = comma + 1;
        }
    }

    // Inverse of the slave's fallback formula, rounded to nearest.
    long delta = ((long)mod.currentPos - (long)mod.homeOffset) %
                 (long)mod.stepsPerRev;
    if (delta < 0) delta += (long)mod.stepsPerRev;
    const int index =
        (int)(((delta * 64L) + (long)mod.stepsPerRev / 2) /
              (long)mod.stepsPerRev) %
        DRUM_SIZE;
    mod.currentFlapIndex = index;
    mod.flap = mod.flapList[index];
}
