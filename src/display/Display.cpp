#include "Display.h"
#include "RS485.h"
#include "WString.h"
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <EEPROM.h>
#include "web/WebSerialConsole.h"
#include "Config.h"

Display splitflap;

// namespace
Display::Display(){}


void Display::begin()
{
    EEPROM.begin(EEPROM_SIZE);
    loadEEpromModules();
    sortModules();
}




bool Display::setModuleChar(uint16_t id, char flap) {
    if (id <= 0 || id >= 256 || flap == '\0') return false;

    // The slave protocol uses m<id>-<char> to move a module to a flap.
    String message;
    message += 'm';
    message += String(id);
    message += '-';
    message += flap;
    rs485Send(message);

    // Give the slave time to consume the line before the next command is
    // placed on the shared bus.
    delay(BUS_RELEASE_MS);

    // Remember the requested flap so the next status payload already reports
    // the new character while the motor is still turning.
    if (Module* mod = getModule(id)) {
        mod->flap = flap;
    }
    return true;
}


