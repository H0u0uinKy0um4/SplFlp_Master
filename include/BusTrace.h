#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

// Who produced a console line. The master registers the source handles once
// and never exposes the web-console implementation through this interface.
enum class BusSource : uint8_t {
    Ha,
    Master,
    Slave,
};

void busTraceBegin();
void busTraceLine(BusSource source, const String& line);
void busTraceChar(BusSource source, char value);
