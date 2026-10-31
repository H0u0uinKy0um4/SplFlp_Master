#include "Display.h"
#include "RS485.h"
#include "WString.h"
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <EEPROM.h>
#include "web/WebSerialConsole.h"
#include "Config.h"



void Display::storeEEpromId(int id) {
    uint8_t count = EEPROM.read(ADDR_STORED_IDS);
    // Guard against uninitialized EEPROM or invalid count
    if (count > 254) count = 0;

    // Check if ID is already stored to prevent duplicates
    for (uint8_t i = 0; i < count; i++) {
        if (EEPROM.read(ADDR_STORED_IDS + 1 + i) == (uint8_t)id) {
            return;
        }
    }

    EEPROM.write(ADDR_STORED_IDS + 1 + count, (uint8_t)id);
    EEPROM.write(ADDR_STORED_IDS, count + 1);
    EEPROM.commit();
    Serial.printf("[EEPROM] Stored module ID: %d (total: %u)\n", id, count + 1);
}


size_t Display::discoverModules(uint16_t from, uint16_t to) {
    // Scanning is an internal operation, even when triggered by HA.
    WebSerialConsole::Scope scope(WebSerial, WebSerialConsole::Source::DEBUG);

    modules.clear();

    // Reset EEPROM count before new scan
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(ADDR_STORED_IDS, 0);
    EEPROM.commit();

    Serial.println("Scanning...");
    unsigned long scanStart = millis();
    for (uint16_t id = from; id <= to; id++) {
        Module mod;
        if (sendQueryAndWait(id, SCAN_TIMEOUT_MS, mod)) {
            modules.push_back(mod);
            storeEEpromId(mod.id);
        }
    }
    sortModules();
    //TimeDebug
    Serial.printf("Scan finished: %u module(s) in %lu ms\n",
                  (unsigned)modules.size(), millis() - scanStart);
    return modules.size();
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
    for (auto &mod : modules)
    {
        if (!sendQueryAndWait(mod.id, SCAN_TIMEOUT_MS, mod))
        {
            mod.online = false;
        }
    }
}

bool Display::sendQueryAndWait(uint16_t id, uint16_t timeoutMs, Module &mod)
{
    String message;
    message += 'm';
    message += String(id);
    message += 'd';

    rs485Reset();
    rs485Send(message);

    unsigned long startTime = millis();
    bool gotConfig = false;

    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        String response = rs485read();
        if (response.length() > 0) {
            // Keep a flap that was already requested over MQTT: the config
            // response does not carry it.
            const char previousFlap = mod.flap;
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
    String drumQuery;
    drumQuery += 'm';
    drumQuery += String(id);
    drumQuery += 'l';

    rs485Reset();
    rs485Send(drumQuery);

    startTime = millis();
    while (millis() - startTime < timeoutMs || rs485RxBusy()) {
        String drum = rs485read(false);
        if (drum.length() > 0) {
            if (drum.length() == DRUM_SIZE) {
                mod.flapList = drum;
            }
            break;
        }
        yield();
    }

    delay(BUS_RELEASE_MS);
    return mod.online;
}

void Display::sortModules() {
    std::sort(modules.begin(), modules.end(),
        [](const Module& a, const Module& b) {
            return a.id < b.id;
        });
}