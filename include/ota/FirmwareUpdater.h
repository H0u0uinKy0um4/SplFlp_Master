#pragma once

#include <cstddef>
#include <cstdint>

// The seam between Master Firmware Session and the ESP32 updater. It lets host
// tests exercise the session policy without a framework, flash or reboot.
class FirmwareUpdater {
  public:
    virtual ~FirmwareUpdater() = default;

    // Maximum bytes that fit the inactive application slot.
    virtual uint32_t capacity() const = 0;
    // Reserve the slot. This does not write image bytes or arm a boot marker.
    virtual bool begin(uint32_t size) = 0;
    // Expected SHA-256 of the whole image, as 64 lowercase/uppercase hex chars.
    virtual bool setExpectedDigest(const char* sha256Hex) = 0;
    virtual size_t write(const uint8_t* data, size_t len) = 0;
    // Verify the digest and only then mark the written slot bootable.
    virtual bool finish() = 0;
    // Discard the partial write; the running application stays bootable.
    virtual void abort() = 0;
    virtual uint32_t written() const = 0;
    virtual const char* error() const = 0;
};
