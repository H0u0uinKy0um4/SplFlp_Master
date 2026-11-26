#pragma once

#include <cstddef>
#include <cstdint>

namespace sf {

// Incremental SHA-256. Kept dependency-free so the same verification code runs
// on the ESP32 and in host tests; no Arduino or mbedTLS types leak into it.
class Sha256 {
  public:
    Sha256() { reset(); }

    void reset();
    void update(const uint8_t* data, size_t len);
    void digest(uint8_t out[32]) const;
    // NUL-terminated lowercase hex, 64 characters plus the terminator.
    void hex(char out[65]) const;

  private:
    void block(const uint8_t bytes[64]);

    uint32_t state_[8] = {};
    uint8_t buffer_[64] = {};
    size_t buffered_ = 0;
    uint64_t bitLength_ = 0;
};

}  // namespace sf
