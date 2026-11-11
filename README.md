# Split-Flap master (ESP32-C3)

The bridge between Home Assistant and the RS485 split-flap modules. It owns the
bus, scans module ids, translates display commands into per-module moves,
publishes the retained status payload, and flashes module firmware over RS485.

The slave-side wire protocol, the OTA bootloader and the cross-repo version
model are owned by the `SplFlp_Slave_Attiny` repository:

- `PROTOCOL.md` - canonical RS485 and OTA contract
- `docs/VERSIONING.md` - compatibility generations and release rules
- `bootloader/README.md` - bootloader design and recovery

This repository implements the master side of those contracts. Do not fork the
protocol here; mirror the constants and link to the source above.

## Hardware and build

| Signal | ESP32-C3 pin | Slave side |
| --- | --- | --- |
| RS485 RX | GPIO5 | module TX (PA5) |
| RS485 TX | GPIO6 | module RX (PA7) |
| RS485 DE | GPIO7 | transceiver DE/RE |
| Baud | 9600 8N1 | same |

```bash
pio run                 # build
pio run -t upload       # flash the ESP32
```

Configuration lives in `include/secrets.h` (WiFi + MQTT) and
`include/Config.h` (pins, timeouts, scan slice). The default MQTT prefix is
`splitflap`.

## Web console

The browser console is not part of this repository any more: it is the
`WebSerialConsole` library, which serves the page, bridges the USB serial port
to the WebSocket and keeps the scrollback.

```ini
lib_deps =
    https://github.com/H0u0uinKy0um4/WebSerialConsole.git#v1.1.1
```

The console is reachable at **http://splitflap.local/** (mDNS) and at the IP
the master prints on the USB console.

The library only knows two output categories (`DEBUG` and `USER`); the master
registers its own in `src/Sources.cpp`, so the web UI can label and colour them:

| Category | Tag | Badge | Colour | Used for |
| --- | --- | --- | --- | --- |
| `ha` | `H` | HA | `#c084fc` | Home Assistant (MQTT) requests |
| `master` | `M` | MST | `#fbbf24` | Commands the master puts on the bus |
| `slave` | `S` | SLV | `#34d399` | Answers the slave modules send back |

```cpp
{
  WebSerialConsole::Scope scope(WebSerial, sourceMaster);
  Serial.println("m1s");     // shown as an MST line in the browser
}
```

## MQTT contract

### Commands (Home Assistant -> master)

| Topic | Payload |
| --- | --- |
| `<prefix>/command/display` | `{"modules":[{"id":1,"char":"A"},...]}`; the array order is the send order |
| `<prefix>/command/sync` | empty; republish status |
| `<prefix>/command/scan` | empty; incremental 1..255 bus scan |
| `<prefix>/command/update` | empty; refresh known modules |
| `<prefix>/command/calibrate` | movement/config actions (`goto`, `home`, `step`, `record`, `apply`, `clear_map`, `drum_write`, `refresh`, `get_pos`) |
| `<prefix>/command/firmware` | OTA image collection and session (`begin`, `chunk`, `flash`, `cancel`, `reset`) |

### Status (master -> Home Assistant, retained)

```json
{
  "state": "online",
  "text": "HELLO",
  "version": "1.1.0.0",
  "bus_compat": 1,
  "mqtt_compat": 1,
  "compat": 1,
  "scanning": false,
  "modules_count": 3,
  "modules": [
    {
      "id": 3,
      "state": "online",
      "online": true,
      "flap": "A",
      "current_step": 1234,
      "current_index": 7,
      "position_known": true,
      "busy": false,
      "total_steps": 4096,
      "home_offset": 2832,
      "auto_homing": true,
      "flap_list": " ABC...",
      "map_of_positions": "0=2832,1=2896,...",
      "fw_version": "1.0.1",
      "fw_bus_compat": 1,
      "fw_bus_compatible": true,
      "fw_compat": 1,
      "fw_compatible": true
    }
  ],
  "ota": {
    "state": "idle"
  }
}
```

`version`/`bus_compat`/`mqtt_compat` are the master's own version contract;
`compat` remains an alias for `mqtt_compat` for older Home Assistant builds.
Per-module `fw_version`/`fw_bus_compat` come from the slave's `m<id>s` reply;
`fw_bus_compatible` is omitted when the module does not report a version, and
`fw_compat`/`fw_compatible` remain aliases for the bus values. During a
flash session `ota` carries `state`, `module`, `progress`, `error` and the
module's `bootloader_version`.

## Module OTA

The master receives a base64 image on `command/firmware`, assembles it, and
then owns the binary framing to the module. The full sequence:

1. `m<id>b` asks the running application to arm the boot flag and reset.
2. PING until the bootloader answers; the OTA protocol generation must match
   `OTA_PROTOCOL_VERSION`, otherwise the session fails **before** any write.
3. START_UPDATE with size + CRC-32 (arms the boot flag).
4. WRITE_PAGE for each 64-byte page with retries.
5. FINISH: the module verifies, clears the flag and resets.

A cancelled session with pages already written leaves the module in the
bootloader for a re-flash. Without pages, the master resets it back into the
intact application. Recovery details are in the slave `PROTOCOL.md`.

## Version

`include/Version.h` is the master's source of truth:

```c
#define MASTER_BUS_COMPAT 1         // shared with the slave (slave <-> master)
#define MASTER_MQTT_COMPAT 1        // shared with Home Assistant (MQTT JSON)
#define MASTER_RELEASE "0.0"        // per-repo release
#define MASTER_VERSION "1.1.0.0"    // <bus>.<mqtt>.<release>
#define OTA_PROTOCOL_VERSION 1      // bootloader framing generation
```

The master carries both compatibility domains because it bridges them. A
compatibility mismatch is visible, not fatal: display traffic keeps working,
HA warns, and only the OTA path is strict because it writes irreversible state.
The OTA protocol generation is a third, independent number; it changes only
with the bootloader framing or boot policy. See the slave's
`docs/VERSIONING.md` for the bump procedure.

## Source map

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Setup and loop |
| `src/display/` | Module discovery, moves, position maps, settled-move refresh |
| `src/RS485.cpp` | Half-duplex line send/receive |
| `src/network/WifiMqttHandler.cpp` | MQTT commands, status payload, discovery |
| `src/ota/SlaveFlasher.cpp` | Image assembly and the blocking RS485 flash session |
| `src/Sources.cpp` | Output categories (HA, bus master, bus slave) of the web console |
| `include/Config.h` | Pins, bus timing, scan limits |
| `include/Sources.h` | Handles of the registered output categories |
| `include/Version.h` | Compatibility/release constants |
