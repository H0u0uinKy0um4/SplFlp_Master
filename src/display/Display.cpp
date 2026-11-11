#include "Display.h"
#include "RS485.h"
#include "WString.h"
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <EEPROM.h>
#include <WebSerialConsole.h>
#include "Config.h"

Display splitflap;

Display::Display(){}


void Display::begin()
{
    EEPROM.begin(EEPROM_SIZE);
    loadEEpromModules();
    sortModules();
}


bool Display::setModuleChar(uint16_t id, char flap) {
    if (id <= 0 || id >= 256 || flap == '\0') return false;

    // A newline or carriage return would terminate the RS485 line early.
    if (flap == '\r' || flap == '\n') flap = ' ';

    Module* mod = getModule(id);

    // The slave's moveToFlap() silently ignores a character that is not on its
    // drum, so reject it here instead of reporting a character that will never
    // be shown.  Only enforce this once the module has reported its drum.
    if (mod && mod->flapList.length() == DRUM_SIZE &&
        mod->flapList.indexOf(flap) < 0) {
        Serial.printf("[Display] Module %u has no flap '%c'\n", id, flap);
        return false;
    }

    // The slave protocol uses m<id>-<char> to move a module to a flap.
    String message;
    message += 'm';
    message += String(id);
    message += '-';
    message += flap;
    rs485Send(message);

    // The slave queues lines while it turns, so commands are never delayed;
    // only queries must wait out the move (see markMovingFlap).
    markMovingFlap(id, flap);

    // Give the slave time to consume the line before the next command is
    // placed on the shared bus.
    delay(BUS_RELEASE_MS);

    // Remember the requested flap so the next status payload already reports
    // the new character while the motor is still turning.
    if (mod) {
        mod->flap = flap;
    }
    return true;
}
