#include "BusTrace.h"
#include <WebSerialConsole.h>

namespace {

WebSerialConsole::Source sourceHa;
WebSerialConsole::Source sourceMaster;
WebSerialConsole::Source sourceSlave;

WebSerialConsole::Source& handle(BusSource source) {
    switch (source) {
        case BusSource::Ha:
            return sourceHa;
        case BusSource::Slave:
            return sourceSlave;
        case BusSource::Master:
        default:
            return sourceMaster;
    }
}

}  // namespace

void busTraceBegin() {
    sourceHa = WebSerial.addSource("ha", 'H', "HA", "#c084fc",
                                   "Home Assistant requests");
    sourceMaster = WebSerial.addSource("master", 'M', "MST", "#fbbf24",
                                       "Outgoing RS485 commands");
    sourceSlave = WebSerial.addSource("slave", 'S', "SLV", "#34d399",
                                      "RS485 slave responses");
}

void busTraceLine(BusSource source, const String& line) {
    WebSerialConsole::Scope scope(WebSerial, handle(source));
    Serial.println(line);
}

void busTraceChar(BusSource source, char value) {
    WebSerialConsole::Scope scope(WebSerial, handle(source));
    Serial.print(value);
}
