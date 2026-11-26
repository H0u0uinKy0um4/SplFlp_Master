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

Module Runtime can be tested on the host without hardware. The `native`
environment compiles only `ModuleRuntime.cpp` and `ContractSnapshot.cpp`
against a fake `LinkAdapter` and a fixed clock; no wire, payload or version
behaviour is simulated there.

```bash
pio test -e native      # 25 Unity tests
```

Configuration lives in `include/secrets.h` (WiFi + MQTT) and
`include/Config.h` (pins, timeouts, scan slice). The default MQTT prefix is
`splitflap`. The build uses `min_spiffs.csv` so the web console and OTA stack
fit in two 1.875 MB application slots on the 4 MB flash.

## Web console

The browser console is not part of this repository any more: it is the
`WebSerialConsole` library, which serves the page, bridges the USB serial port
to the WebSocket and keeps the scrollback.

```ini
lib_deps =
    https://github.com/H0u0uinKy0um4/WebSerialConsole.git#v1.1.2
```

The console is reachable at **http://splitflap.local/** (mDNS) and at the IP
the master prints on the USB console.

The library only knows two output categories (`DEBUG` and `USER`); the master
registers its own in `src/BusTrace.cpp`, so the web UI can label and colour them:

| Category | Tag | Badge | Colour | Used for |
| --- | --- | --- | --- | --- |
| `ha` | `H` | HA | `#c084fc` | Home Assistant (MQTT) requests |
| `master` | `M` | MST | `#fbbf24` | Commands the master puts on the bus |
| `slave` | `S` | SLV | `#34d399` | Answers the slave modules send back |

```cpp
busTraceLine(BusSource::Master, "m1s");  // shown as an MST line in the browser
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
| `<prefix>/command/firmware` | Module OTA image collection and session (`begin`, `chunk`, `flash`, `cancel`, `reset`) |
| `<prefix>/command/master_firmware` | Master self-update (`begin`, `chunk`, `flash`, `cancel`) |

### Status (master -> Home Assistant, retained)

```json
{
  "state": "online",
  "text": "HELLO",
  "version": "1.2.0.0",
  "bus_compat": 1,
  "mqtt_compat": 2,
  "compat": 2,
  "ip": "10.0.1.15",
  "master_ota": {
    "state": "idle",
    "progress": 0,
    "received": 0,
    "size": 0,
    "version": "1.2.0.0",
    "target_version": "",
    "error": ""
  },
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
module's `bootloader_version`. `ip` is additive and exists for ArduinoOTA
recovery. `master_ota` is always present for the master's own self-update; its
states are `idle`, `collecting`, `applying`, `rebooting`, `done`, `failed` and
`cancelled`. `version` is the running master, `target_version` the image being
collected.

## Master self-update

The master flashes its own application through the same retained status flow:

1. `{"action":"begin","version":"...","sha256":"<64 hex>","size":N}` validates
   the target and reserves the inactive OTA slot.
2. `{"action":"chunk","data":"<base64>"}` decodes straight into that slot while
   hashing incrementally. No staging file is used: the 4 MB board has only a
   128 KB data partition. See `docs/adr/0001-master-ota-streaming.md`.
3. `{"action":"flash"}` verifies the digest and arms the boot marker only after
   the whole image passes; `{"action":"cancel"}` discards a partial collection.
   The device reboots on success.

A size/digest mismatch or a cancelled image never becomes bootable. Display,
scan, update and calibrate are refused only while `flash` is applying, not while
chunks are being collected. A bad first boot rolls back automatically. Manual
recovery stays ArduinoOTA on port 3232 (`SplFlp_Master`, `OTA_PASSWORD`).

To publish the matching release assets:

```bash
pio run -e esp32-c3-devkitm-1
python3 scripts/make-release-assets.py
gh release create mqtt-compat-2 \
  .pio/build/esp32-c3-devkitm-1/firmware.bin \
  .pio/build/esp32-c3-devkitm-1/firmware.json \
  --title "MQTT compat 2" --notes "Master app for the coordinated MQTT compat 2 release"
```

`firmware.json` carries `kind: "master-app"`, `schema`, `version`,
`mqtt_compat`, `bus_compat` (diagnostic only), `image`, `sha256`, `channel` and
`notes`, all derived from `include/Version.h` and the built image.

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

### First OTA from firmware older than 0.1

A device still reporting status `version: "1.1.0.0"` was built with the default
1.25 MB application partition. The current image is ~1.45 MB, so OTA is
rejected on the device with `OTA Error[1]: Could not begin update` and the host
only sees `No response from device`. The partition table cannot be changed by
OTA: flash the current build **once over USB** to install the two 1.875 MB
slots, then OTA works normally.

```bash
# with the ESP32-C3 connected on /dev/ttyACM0
~/.platformio/packages/tool-esptool/esptool --port /dev/ttyACM0 --baud 115200 \
  write_flash \
  0x0     .pio/build/esp32-c3-devkitm-1/bootloader.bin \
  0x8000  .pio/build/esp32-c3-devkitm-1/partitions.bin \
  0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
```

OTA of this image takes about two minutes. Do not interrupt it, and avoid
keeping a browser console WebSocket open during the transfer: progress logging
and a flaky Wi-Fi link can both turn a partial transfer into
`OTA Error[3]: Firmware receive failed`.

## Version

`include/Version.h` is the master's source of truth:

```c
#define MASTER_BUS_COMPAT 1         // shared with the slave (slave <-> master)
#define MASTER_MQTT_COMPAT 2        // shared with Home Assistant (MQTT JSON)
#define MASTER_RELEASE "0.0"        // reset for the MQTT compat 2 generation
#define MASTER_VERSION "1.2.0.0"    // <bus>.<mqtt>.<release>
#define OTA_PROTOCOL_VERSION 1      // bootloader framing generation
```

The master carries both compatibility domains because it bridges them. A
compatibility mismatch is visible, not fatal: display traffic keeps working,
HA warns, and only the OTA path is strict because it writes irreversible state.
The OTA protocol generation is a third, independent number; it changes only
with the bootloader framing or boot policy. The `mqtt-compat-2` tag coordinates
this repository with the slave and Home Assistant releases. See the slave's
`docs/VERSIONING.md` for the bump procedure.

## Source map

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | Setup and loop |
| `src/BusTrace.cpp` | Console source registration and tagged line/byte emission |
| `src/Rs485Link.cpp` | Single owner of DE, echo, ASCII lines and binary frames |
| `include/LinkAdapter.h` | Link seam: real RS485 adapter and the native-test fake |
| `src/ModuleRuntime.cpp` | All Slave Module effects, scan, move windows, readback and one tick seam |
| `src/ContractSnapshot.cpp` | Version composition, state reply parsing and legacy aliases |
| `src/MasterControl.cpp` | MQTT command interpretation, bus effects and Status Payload |
| `src/network/WifiMqttHandler.cpp` | WiFi/MQTT transport adapter and discovery |
| `src/ModuleFirmwareSession.cpp` | Slave Module image assembly, bootloader session, recovery |
| `src/ota/MasterFirmwareSession.cpp` | Master self-update state machine, streaming and verification |
| `src/ota/Esp32FirmwareUpdater.cpp` | ESP32 `Update` adapter for the inactive OTA slot |
| `src/ota/Sha256.cpp` | Dependency-free incremental SHA-256 shared with host tests |
| `src/network/OtaHandler.cpp` | ESP32 ArduinoOTA adapter |
| `scripts/make-release-assets.py` | Build `firmware.json` from `Version.h` and `firmware.bin` |
| `include/Config.h` | Pins, bus timing, scan limits |
| `include/Version.h` | Compatibility/release constants |
| `GLOSSARY.md` | Domain vocabulary used by the master modules |
