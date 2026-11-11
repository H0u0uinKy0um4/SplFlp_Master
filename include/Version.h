#pragma once

// Version model (canonical description in the slave repository's
// docs/VERSIONING.md):
//
//   COMPAT   one integer shared by slave app, master and Home Assistant.
//            It changes only for a breaking cross-repo contract change.
//   RELEASE  this repository's own release inside that generation.
//
// The published version is "<COMPAT>.<RELEASE>", so the compatibility
// generation is readable from the first number alone.

#define SF_STRINGIFY_(value) #value
#define SF_STRINGIFY(value) SF_STRINGIFY_(value)

// Bump together with FW_COMPAT (slave include/FirmwareVersion.h) and
// COMPAT_VERSION (HA const.py).
#define MASTER_COMPAT 1

// Bump for every master release; independent of the slave and HA releases.
#define MASTER_RELEASE "0.0"

#define MASTER_VERSION SF_STRINGIFY(MASTER_COMPAT) "." MASTER_RELEASE

// OTA binary protocol generation implemented by SlaveFlasher. This is
// separate from the application compatibility generation: it changes only
// when the bootloader framing, commands or boot policy change. It must match
// BOOTLOADER_VERSION in the slave's bootloader/src/protocol.h.
#define OTA_PROTOCOL_VERSION 1

// A module whose application reports another generation still works for
// display traffic (the ASCII protocol is append-only), but HA warns. The OTA
// flasher is stricter: it refuses to write anything until the bootloader
// reports OTA_PROTOCOL_VERSION.
#define SLAVE_COMPAT MASTER_COMPAT
