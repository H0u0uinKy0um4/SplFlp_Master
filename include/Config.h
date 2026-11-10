#pragma once

#define RS485_RX 5
#define RS485_TX 6
#define RS485_DE 7
#define RS485_BAUD 9600

// Number of flap positions on the stock drum. The slave always reports a
// 64-character drum (including the leading blank flap).
#define DRUM_SIZE 64

#define ID_BEGIN 1
#define ID_END 255

// The slave answers a query within a few milliseconds when idle. 40 ms leaves
// a safe margin. This timeout is paid for every empty ID, so it dominates the
// total scan time (255 IDs x 40 ms ~= 10 s).
#define SCAN_TIMEOUT_MS 40
constexpr unsigned long BUS_RELEASE_MS = 10;

// Bus time the incremental scanner may spend per loop pass. A full sweep
// costs ~10 s of bus time; probing it in slices keeps MQTT, the web console
// and OTA responsive while the bus is being walked.
constexpr unsigned long SCAN_SLICE_MS = 30;

// Worst-case time for one slave move (one full revolution at 1 ms/half-step)
// plus a margin.  Used to defer the refresh after a movement command so the
// module is not queried while it is still turning.
constexpr unsigned long MOVE_SETTLE_MS = 5000;

// Extra time added to every estimated move window so the query lands after
// the move with certainty (motor spin-up, EEPROM writes, parse time).
constexpr unsigned long MOVE_MARGIN_MS = 250;;