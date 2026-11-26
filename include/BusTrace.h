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

// Incoming firmware chunks are base64 blobs many kilobytes long. Copying them
// into the console path (and the browser scrollback) can exhaust the heap in
// the MQTT callback, so log only the envelope for oversized commands.
constexpr size_t MAX_LOGGED_COMMAND_LENGTH = 256;

inline String formatCommandTrace(const String& topic, const String& message) {
    if (message.length() > MAX_LOGGED_COMMAND_LENGTH) {
        return String("<- ") + topic + " [" + String(message.length()) +
               " bytes, payload suppressed]";
    }
    return String("<- ") + topic + " " + message;
}
