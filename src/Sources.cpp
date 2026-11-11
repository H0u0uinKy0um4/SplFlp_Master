#include "Sources.h"

// Home Assistant requests (MQTT commands).
WebSerialConsole::Source sourceHa;
// Commands the master puts on the RS485 bus.
WebSerialConsole::Source sourceMaster;
// Answers the slave modules send back.
WebSerialConsole::Source sourceSlave;

void registerSources() {
  sourceHa = WebSerial.addSource("ha", 'H', "HA", "#c084fc",
                                  "Home Assistant requests");
  sourceMaster = WebSerial.addSource("master", 'M', "MST", "#fbbf24",
                                      "Outgoing RS485 commands");
  sourceSlave = WebSerial.addSource("slave", 'S', "SLV", "#34d399",
                                     "RS485 slave responses");
}
