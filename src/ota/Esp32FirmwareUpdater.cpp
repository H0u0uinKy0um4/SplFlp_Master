#include "ota/Esp32FirmwareUpdater.h"

#include <Update.h>
#include <cstring>
#include <esp_ota_ops.h>

Esp32FirmwareUpdater esp32FirmwareUpdater;

void Esp32FirmwareUpdater::setError(const char* message) {
    error_[0] = '\0';
    if (message != nullptr) {
        strncpy(error_, message, sizeof(error_) - 1);
    }
}

uint32_t Esp32FirmwareUpdater::capacity() const {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    return partition != nullptr ? partition->size : 0;
}

bool Esp32FirmwareUpdater::begin(uint32_t size) {
    setError(nullptr);
    if (!Update.begin(size, U_FLASH)) {
        setError(Update.errorString());
        Update.abort();
        return false;
    }
    return true;
}

bool Esp32FirmwareUpdater::setExpectedDigest(const char* sha256Hex) {
    if (!Update.setSHA256(sha256Hex)) {
        setError("Update rejected the expected SHA-256");
        return false;
    }
    return true;
}

size_t Esp32FirmwareUpdater::write(const uint8_t* data, size_t len) {
    const size_t written = Update.write(const_cast<uint8_t*>(data), len);
    if (written != len) setError(Update.errorString());
    return written;
}

bool Esp32FirmwareUpdater::finish() {
    // end() checks the SHA-256 and only then calls esp_ota_set_boot_partition().
    if (!Update.end()) {
        setError(Update.errorString());
        Update.abort();
        return false;
    }
    return true;
}

void Esp32FirmwareUpdater::abort() {
    Update.abort();
}

uint32_t Esp32FirmwareUpdater::written() const {
    return Update.progress();
}
