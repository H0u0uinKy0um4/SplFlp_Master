# Split-Flap Master

The Split-Flap Master bridges Home Assistant over MQTT and the physical slave modules over RS485. It owns display commands, module discovery, calibration and module firmware updates.

## Language

**RS485 Bus**:
The half-duplex bus shared by the master and every **Slave Module**. Commands and replies travel as lines; module firmware updates use a separate binary framing inside the same bus.
_Avoid_: serial, wire

**Slave Module**:
One physical split-flap module on the **RS485 Bus**, addressed by a one-byte id. It reports its position, drum, movement state and firmware version.
_Avoid_: device, node, unit

**Display Command**:
An ordered set of requested flap characters for one or more **Slave Modules**, sent in the order the caller supplied.
_Avoid_: text command, write

**Firmware Image**:
The raw application bytes for one **Slave Module**, assembled from chunks and verified with a CRC-32 before it is written.
_Avoid_: binary, blob

**Bootloader**:
The resident program on a **Slave Module** that accepts a **Firmware Image** and controls when the updated application may boot.
_Avoid_: updater, flasher

**Status Payload**:
The retained MQTT document that reports master state, every known **Slave Module**, and the current module firmware session to Home Assistant.
_Avoid_: state message, telemetry

**RS485 Link**:
The single module that owns DE timing, echo suppression, line buffering, binary frames and console tracing for the **RS485 Bus**.
_Avoid_: serial driver, transport layer

**Bus Trace**:
The module that owns source registration and emits tagged console lines for Home Assistant requests, master bus commands and **Slave Module** replies.
_Avoid_: logger, debug output

**Module Runtime**:
The module that owns the live record of every **Slave Module**, its move windows, scan progress and post-move readback.
_Avoid_: display object, module cache

**Contract Snapshot**:
The module that turns version constants and peer replies into the fields and compatibility aliases published in the **Status Payload**.
_Avoid_: version helper, compat parser

**Master Control**:
The module that interprets Home Assistant commands, applies their effects on the **Module Runtime** or **Module Firmware Session**, and renders the **Status Payload**.
_Avoid_: MQTT callback, command handler

**Module Firmware Session**:
One image-assembly and bootloader exchange for a **Slave Module**, including boot policy, retry, cancellation and recovery of its **Display Command** state.
_Avoid_: OTA flasher, updater
