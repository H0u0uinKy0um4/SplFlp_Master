#pragma once

#include <WebSerialConsole.h>

// Output categories of this firmware.
//
// The console library only defines two built-in categories (DEBUG and USER);
// everything the master distinguishes on the network or on the RS485 bus is
// registered here, with the wire tag, the badge label and the colour the web
// console uses.
//
// The handles are filled in by registerSources(), which setup() calls before
// anything is logged.
extern WebSerialConsole::Source sourceHa;
extern WebSerialConsole::Source sourceMaster;
extern WebSerialConsole::Source sourceSlave;

// Register the application-specific output categories.
void registerSources();
