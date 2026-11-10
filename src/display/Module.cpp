
#include "Display.h"
#include "RS485.h"
#include <Arduino.h>
#include <cstdint>
#include "Config.h"
#include "WString.h"


Module::Module(const String& s) {
    int start = s.indexOf(':');

    if (start < 0) return;
    id = s.substring(1, start).toInt();

    // The slave's m<id>d response is:
    //   m<id>:<homeOffset>:<totalSteps>:<autoHome>:<index>=<pos>,...
    // The position map is optional and may be empty, but the colon before it
    // is always present in current firmware.
    int cursor = start + 1;
    auto takeField = [&s, &cursor]() -> String {
        int colon = s.indexOf(':', cursor);
        String value =
            colon < 0 ? s.substring(cursor) : s.substring(cursor, colon);
        cursor = colon < 0 ? s.length() + 1 : colon + 1;
        return value;
    };

    homeOffset = takeField().toInt();
    uint32_t parsedSteps = takeField().toInt();
    if (parsedSteps > 0) stepsPerRev = parsedSteps;
    autoHome = takeField().toInt() != 0;
    status = autoHome ? 1 : 0;

    if (cursor <= static_cast<int>(s.length())) {
        positionMap = s.substring(cursor);
    }

    online = true;
}


Module::Module() {
}
