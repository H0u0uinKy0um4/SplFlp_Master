#include "Display.h"
#include "RS485.h"
#include "WString.h"
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <EEPROM.h>
#include <WebSerialConsole.h>
#include "Config.h"


void Display::storeEEpromId(int id) {
    uint8_t count = EEPROM.read(ADDR_STORED_IDS);
    // Guard against uninitialized EEPROM or invalid count
    if (count > 254) count = 0;

    // The EEPROM format stores the count at address 0 and ids at 1..254, so
    // 254 is the hard maximum. Stopping here loses one id at most, while the
    // old code stored 255, which the boot guard above treated as "empty".
    if (count >= 254) {
        Serial.println("[EEPROM] Module ID table full, not storing");
        return;
    }

    // Check if ID is already stored to prevent duplicates
    for (uint8_t i = 0; i < count; i++) {
        if (EEPROM.read(ADDR_STORED_IDS + 1 + i) == (uint8_t)id) {
            return;
        }
    }

    EEPROM.write(ADDR_STORED_IDS + 1 + count, (uint8_t)id);
    EEPROM.write(ADDR_STORED_IDS, count + 1);
    // The commit is deferred to the end of the scan: every commit rewrites
    // the whole flash sector, so one per module needlessly wears it.
    Serial.printf("[EEPROM] Queued module ID: %d (total: %u)\n", id, count + 1);
}


// ---------------------------------------------------------------------------
// Incremental bus scan
// ---------------------------------------------------------------------------

void Display::startScan(uint16_t from, uint16_t to) {
    // Clamp to the real bus range; an inverted or empty range is a no-op.
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

    // Reset the EEPROM id table; the single commit in finishScan() persists
    // the count and every queued id together.
    EEPROM.write(ADDR_STORED_IDS, 0);

    // The live module list is deliberately NOT cleared here: display and
    // calibrate commands keep being served while the scan runs, and a module
    // that is mid-move cannot answer a probe. Modules that do not answer
    // (and were not busy-known) are dropped in finishScan() instead.
    Serial.printf("Scanning ids %u..%u...\n", (unsigned)from, (unsigned)to);
}

bool Display::scanStep() {
    if (!scanning) return false;

    const unsigned long sliceStart = millis();
    while (scanNext <= scanTo && millis() - sliceStart < SCAN_SLICE_MS) {
        // Probe with the live record when the id is already known, so an
        // active move window is honoured: a turning module cannot answer,
        // but it is definitionally present, so it is counted as found.
        Module* live = getModule(scanNext);
        Module fresh;
        Module& probe = live ? *live : fresh;
        if (sendQueryAndWait(scanNext, SCAN_TIMEOUT_MS, probe)) {
            if (!live && !modules.full()) modules.push_back(fresh);
            markScanIdFound(scanNext);
            storeEEpromId(scanNext);
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

void Display::finishScan() {
    scanning = false;

    // Drop every module that did not answer this sweep (busy-known modules
    // were marked found in scanStep and survive with their cached state).
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
    Serial.printf("Scan finished: %u module(s) in %lu ms\n",
                  (unsigned)modules.size(), millis() - scanStartMs);
}

bool Display::isScanning() const {
    return scanning;
}

int Display::scanProgress() const {
    if (!scanning) return -1;
    const uint16_t total = scanTo - scanFrom + 1;
    if (total == 0) return 100;
    return (int)((unsigned long)scanProbed * 100UL / total);
}

bool Display::scanIdFound(uint16_t id) const {
    if (id > ID_END) return false;
    return (scanFoundMask[id >> 3] & (1 << (id & 7))) != 0;
}

void Display::markScanIdFound(uint16_t id) {
    if (id > ID_END) return;
    scanFoundMask[id >> 3] |= (1 << (id & 7));
}

void Display::loadEEpromModules()
{
    uint8_t count = EEPROM.read(ADDR_STORED_IDS);
    // Guard against uninitialized EEPROM or invalid count
    if (count > 254) count = 0;
    for (uint8_t i = 0; i < count; i++) {
        Module mod;
        mod.id = EEPROM.read(ADDR_STORED_IDS + 1 + i);
        sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod);
        modules.push_back(mod);
    }
}

void Display::updateModules()
{
    // Re-query every module we already know about. Modules that no longer
    // answer stay in the list but are marked offline so Home Assistant can
    // show them as missing instead of keeping stale diagnostics forever.
    // Modules inside their move window are skipped by sendQueryAndWait and
    // keep their cached state, so a move never looks like a dropout.
    for (auto &mod : modules)
    {
        if (!sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod))
        {
            mod.online = false;
        }
    }
}

bool Display::refreshModule(uint16_t id)
{
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
// Move windows
// ---------------------------------------------------------------------------

// Estimated landing position of flap `index`, mirroring the slave's fallback
// for an unset map entry (homeOffset + round(index * steps / 64)). It is only
// used to estimate a move's duration, so the missing per-flap corrections do
// not matter.
long Display::flapTargetPos(const Module& mod, int index) const {
    if (mod.stepsPerRev == 0 || index < 0 || index >= DRUM_SIZE) return -1;
    return ((long)mod.homeOffset +
            ((long)index * (long)mod.stepsPerRev + 32) / 64) %
           (long)mod.stepsPerRev;
}

void Display::extendBusy(Module& mod, unsigned long estMs) {
    const unsigned long now = millis();
    // A command sent while a previous move is queued behind the running one
    // extends the deadline instead of replacing it.
    if (mod.busyUntilMs != 0 && (long)(mod.busyUntilMs - now) > 0) {
        mod.busyUntilMs += estMs;
    } else {
        mod.busyUntilMs = now + estMs;
    }
    mod.busyMisses = 0;
}

void Display::markMovingFlap(uint16_t id, char flap) {
    Module* mod = getModule(id);
    if (!mod) return;

    int index = mod->flapList.length() == DRUM_SIZE
                    ? mod->flapList.indexOf(flap)
                    : -1;
    long target = flapTargetPos(*mod, index);
    if (target < 0) {
        // Unknown drum or config: assume the worst case, one revolution.
        extendBusy(*mod, MOVE_SETTLE_MS);
        return;
    }
    markMovingPos(id, target);
}

void Display::markMovingIndex(uint16_t id, int index) {
    Module* mod = getModule(id);
    if (!mod) return;

    long target = flapTargetPos(*mod, index);
    if (target < 0) {
        extendBusy(*mod, MOVE_SETTLE_MS);
        return;
    }
    markMovingPos(id, target);
}

void Display::markMovingPos(uint16_t id, long pos) {
    Module* mod = getModule(id);
    if (!mod) return;

    if (mod->stepsPerRev == 0 || pos < 0) {
        extendBusy(*mod, MOVE_SETTLE_MS);
        return;
    }

    // Forward distance in half-steps; the slave spends ~1 ms per half-step.
    long dist = (pos - (long)mod->currentPos) % (long)mod->stepsPerRev;
    if (dist < 0) dist += (long)mod->stepsPerRev;
    // currentPos is the last reported position, so the estimate can be off
    // by at most a revolution. Cap it there rather than trusting it blindly.
    if (dist > (long)mod->stepsPerRev) dist = (long)mod->stepsPerRev;
    extendBusy(*mod, (unsigned long)dist + MOVE_MARGIN_MS);
}

void Display::markMovingHome(uint16_t id) {
    Module* mod = getModule(id);
    if (!mod) return;

    // Homing leaves the magnet, seeks the entering edge (up to one
    // revolution) and then runs homeOffset steps to flap 0.
    unsigned long est = mod->stepsPerRev > 0
                            ? mod->stepsPerRev + mod->homeOffset
                            : MOVE_SETTLE_MS;
    extendBusy(*mod, est + MOVE_MARGIN_MS);
}

bool Display::refreshSettledMoves() {
    const unsigned long now = millis();
    for (auto& mod : modules) {
        if (mod.busyUntilMs == 0) continue;
        if ((long)(now - mod.busyUntilMs) < 0) continue;   // still moving

        mod.busyUntilMs = 0;
        if (sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod)) {
            mod.busyMisses = 0;
        } else if (++mod.busyMisses < 2) {
            // The move outlived its estimate (or the module went silent).
            // Give it one more window before declaring it offline.
            mod.busyUntilMs = millis() + MOVE_SETTLE_MS;
        } else {
            mod.online = false;
            mod.busyMisses = 0;
        }
        // One module per loop pass keeps the bus, MQTT and the web console
        // responsive while a backlog drains.
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bus queries
// ---------------------------------------------------------------------------

bool Display::sendQueryAndWait(uint16_t id, uint16_t timeoutMs, Module &mod)
{
    // A module inside its move window cannot answer: the slave blocks in its
    // step loop while it turns (PROTOCOL.md, "Motion"). Keep the cached state
    // instead of timing out and reporting the module offline.
    if (mod.busyUntilMs != 0 && (long)(millis() - mod.busyUntilMs) < 0) {
        return mod.online;
    }

    String message;
    message += 'm';
    message += String(id);
    message += 'd';

    rs485Reset();
    rs485Send(message);

    // Only accept a response that carries the id we asked for. A late reply
    // from another module (or a leftover line) must never overwrite this
    // module's record.
    const String configPrefix = "m" + String(id) + ":";

    unsigned long startTime = millis();
    bool gotConfig = false;

    // Keep diagnostics that this query does not carry so a transient failure
    // cannot wipe the cached drum or position.
    const char previousFlap = mod.flap;
    const String previousFlapList = mod.flapList;
    const String previousFwVersion = mod.fwVersion;
    const uint8_t previousFwBusCompat = mod.fwBusCompat;
    const uint32_t previousPos = mod.currentPos;
    const bool previousPositionKnown = mod.positionKnown;

    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        String response = rs485read();
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

    // The drum characters are not part of the config response, so ask for them
    // separately. Read this line untrimmed because the stock drum starts with a
    // blank flap (space) and trimming would shift every flap index.
    // The slave's drum response carries no id prefix, so the length check is
    // the only validation available.
    String drumQuery;
    drumQuery += 'm';
    drumQuery += String(id);
    drumQuery += 'l';

    rs485Reset();
    rs485Send(drumQuery);

    startTime = millis();
    bool gotDrum = false;
    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        String drum = rs485read(false);
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

    // State query: m<id>s:<pos>:<busy>:<position_known>[:<bus_compat>:<fw_version>]
    String stateQuery;
    stateQuery += 'm';
    stateQuery += String(id);
    stateQuery += 's';

    rs485Reset();
    rs485Send(stateQuery);

    const String statePrefix = "m" + String(id) + "s:";
    startTime = millis();
    bool gotState = false;
    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        String resp = rs485read();
        if (resp.length() > 0 && resp.startsWith(statePrefix)) {
            int p1 = resp.indexOf(':');
            int p2 = p1 >= 0 ? resp.indexOf(':', p1 + 1) : -1;
            int p3 = p2 >= 0 ? resp.indexOf(':', p2 + 1) : -1;
            int p4 = p3 >= 0 ? resp.indexOf(':', p3 + 1) : -1;
            int p5 = p4 >= 0 ? resp.indexOf(':', p4 + 1) : -1;
            if (p1 > 0 && p2 > p1) {
                mod.currentPos = (uint32_t)resp.substring(p1 + 1, p2).toInt();
                // Older slaves stop after pos; treat that as position known.
                mod.positionKnown =
                    p3 > p2 ? resp.substring(p3 + 1, p4 > p3 ? p4 : resp.length()).toInt() != 0 : true;
                // Newer slaves append "<bus_compat>:<fw_version>". The
                // bus compat field is explicit so the master never has to
                // parse it out of the version string. The MQTT generation is
                // a separate axis and never appears on this bus.
                if (p4 > p3) {
                    String compat = resp.substring(p4 + 1, p5 > p4 ? p5 : resp.length());
                    compat.trim();
                    if (compat.length() > 0) mod.fwBusCompat = (uint8_t)compat.toInt();
                }
                if (p5 > p4) {
                    String version = resp.substring(p5 + 1);
                    version.trim();
                    if (version.length() > 0) mod.fwVersion = version;
                }
                if (mod.fwVersion.length() == 0) mod.fwVersion = previousFwVersion;
                if (mod.fwBusCompat == 0) mod.fwBusCompat = previousFwBusCompat;
                gotState = true;
            }
            break;
        }
        yield();
    }
    if (!gotState) {
        mod.currentPos = previousPos;
        mod.positionKnown = previousPositionKnown;
        mod.fwVersion = previousFwVersion;
        mod.fwBusCompat = previousFwBusCompat;
    } else {
        // The position is trustworthy: derive the module's real flap from it
        // instead of reporting the last requested character.
        deriveActualFlap(mod);
    }

    delay(BUS_RELEASE_MS);
    return mod.online;
}

// ---------------------------------------------------------------------------
// Actual flap readback
// ---------------------------------------------------------------------------

// Map the reported step position back to a drum index and flap character.
// Prefers an exact position-map entry; falls back to the inverse of the
// slave's linear default (homeOffset + round(index * steps / 64)). Only
// trusted while the module reports position_known: on old slave firmware
// (no mid-move hall re-referencing) an open-loop position can drift, and a
// wrong revolution count must not overwrite the requested character.
void Display::deriveActualFlap(Module& mod) const {
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
                const int index =
                    mod.positionMap.substring(cursor, eq).toInt();
                const long pos =
                    mod.positionMap.substring(eq + 1, end).toInt();
                // 65535 is the slave's "entry unset" sentinel.
                if (index >= 0 && index < DRUM_SIZE &&
                    pos >= 0 && pos < (long)mod.stepsPerRev &&
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
    const int index = (int)(((delta * 64L) + (long)mod.stepsPerRev / 2) /
                            (long)mod.stepsPerRev) %
                     DRUM_SIZE;
    mod.currentFlapIndex = index;
    mod.flap = mod.flapList[index];
}

void Display::sortModules() {
    std::sort(modules.begin(), modules.end(),
        [](const Module& a, const Module& b) {
            return a.id < b.id;
        });
}
