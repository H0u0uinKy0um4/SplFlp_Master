#pragma once

// Host stand-in for the ESP32 EEPROM object. The native tests never load
// records from it; it exists so ModuleRuntime can compile unchanged.
#include <array>
#include <cstddef>
#include <cstdint>

class EepromShim {
  public:
    void begin(size_t) {}
    uint8_t read(int address) const { return data[address]; }
    void write(int address, uint8_t value) { data[address] = value; }
    void commit() {}

  private:
    std::array<uint8_t, 512> data{};
};

extern EepromShim EEPROM;
