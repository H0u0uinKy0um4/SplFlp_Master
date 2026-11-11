#pragma once

// Version model (canonical description in the slave repository's
// docs/VERSIONING.md):
//
// The system has two independent compatibility domains:
//
//   BUS_COMPAT  the slave <-> master side: the RS485 application protocol and
//               the bootloader/OTA entry contract.
//   MQTT_COMPAT the master <-> Home Assistant side: the MQTT status/command
//               JSON schema.
//
// The master is the bridge, so it carries both numbers. RELEASE is this
// repository's own release inside those generations.
//
// The published version is "<bus>.<mqtt>.<release>", e.g. 1.1.0.0. The status
// payload repeats both generations explicitly, so Home Assistant never has to
// parse the string.

#define SF_STRINGIFY_(value) #value
#define SF_STRINGIFY(value) SF_STRINGIFY_(value)

// Bump together with FW_BUS_COMPAT (slave include/FirmwareVersion.h).
#define MASTER_BUS_COMPAT 1

// Bump together with MQTT_COMPAT (HA const.py).
#define MASTER_MQTT_COMPAT 1

// Bump for every master release; independent of the slave and HA releases.
#define MASTER_RELEASE "0.0"

#define MASTER_VERSION SF_STRINGIFY(MASTER_BUS_COMPAT) "." \
                       SF_STRINGIFY(MASTER_MQTT_COMPAT) "." MASTER_RELEASE

// OTA binary protocol generation implemented by SlaveFlasher. This is
// separate from both application compatibility generations: it changes only
// when the bootloader framing, commands or boot policy change. It must match
// BOOTLOADER_VERSION in the slave's bootloader/src/protocol.h.
#define OTA_PROTOCOL_VERSION 1

// A module whose application reports another bus generation still works for
// display traffic (the ASCII protocol is append-only), but HA warns. The OTA
// flasher is stricter: it refuses to write anything until the bootloader
// reports OTA_PROTOCOL_VERSION.
#define SLAVE_BUS_COMPAT MASTER_BUS_COMPAT
