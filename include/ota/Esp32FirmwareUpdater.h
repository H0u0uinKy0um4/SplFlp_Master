#pragma once

#include "ota/FirmwareUpdater.h"

// Thin adapter over Arduino's UpdateClass. Update selects the inactive OTA
// partition, verifies the configured SHA-256 in end(), and arms the boot marker
// only after that verification succeeds.
class Esp32FirmwareUpdater final : public FirmwareUpdater {
  public:
    uint32_t capacity() const override;
    bool begin(uint32_t size) override;
    bool setExpectedDigest(const char* sha256Hex) override;
    size_t write(const uint8_t* data, size_t len) override;
    bool finish() override;
    void abort() override;
    uint32_t written() const override;
    const char* error() const override { return error_; }

  private:
    void setError(const char* message);

    char error_[96] = {};
};

extern Esp32FirmwareUpdater esp32FirmwareUpdater;
