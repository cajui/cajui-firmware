// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef CAJUI_RUNTIME_ROLE
#include <esp_ota_ops.h>
#include "cajui_firmware.h"

namespace board {
constexpr uint32_t FirmwareVersion = CAJUI_FIRMWARE_VERSION;
// Writes an update into the application slot that is not running. Nothing becomes
// bootable before commit(), which validates the image and selects it for the next boot.
class OtaSink final : public cajui::ImageSink {
public:
    bool begin(size_t size) override;
    bool write(const uint8_t* data, size_t size) override;
    bool commit() override;
    void abort() override;
    static size_t capacity(); // Size of the slot an update would use.

private:
    const esp_partition_t* target_ = nullptr;
    esp_ota_handle_t handle_ = 0;
    bool open_ = false;
};
// Logs version, running slot and whether a previous update was rolled back.
void reportFirmware();
// A new image stays on probation after an update: any restart before this call (a crash,
// the watchdog, a fault restart) makes the bootloader return to the previous image.
void confirmFirmware();
} // namespace board
#endif
