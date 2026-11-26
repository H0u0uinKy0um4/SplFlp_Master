# ADR-0001: Stream master firmware into the inactive OTA slot

Status: Accepted (2026-11-26)

## Context

Issue #2 specified a LittleFS staging file for the master self-update. The
board has 4 MB of flash and uses `min_spiffs.csv` to keep two 1.875 MB
application slots, because the current application is ~1.45 MB and ArduinoOTA
must keep working after an OTA. That leaves a 128 KB data partition, so a
staging file cannot hold a complete image.

## Decision

`Master Firmware Session` streams base64-decoded chunks straight into the
inactive OTA slot through `FirmwareUpdater`. It validates the declared size and
SHA-256 format at `begin`, hashes every byte as it arrives, and aborts the
update on a size or digest mismatch. The updater verifies the digest again in
`finish()` and arms the boot marker only after that check passes. A cancelled
collection discards the partial write without touching the running slot.

## Consequences

- The literal staging-file step in issue #2 is replaced by direct streaming.
  This was an explicit user decision after the partition conflict was found.
- Invalid size/digest **format** and concurrent sessions are rejected before
  any slot write. A digest *mismatch* is known only once all bytes arrive; it
  is handled by aborting before the boot marker, so the old slot keeps booting.
- Collection is non-blocking: display, scan, update and calibrate are refused
  only while an image is being applied.
- Cancel is effective while collecting; once `flash` has started, the device is
  committed until the updater finishes or the chip resets.
- ESP-IDF application rollback (enabled in the framework sdkconfig) handles a
  bad first boot. ArduinoOTA on port 3232 remains the manual recovery path.
