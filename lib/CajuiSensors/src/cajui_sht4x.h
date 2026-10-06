// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace cajui {
// Small blocking I2C transaction, bounded by the board adapter's timeout. No hardware
// globals: failures and sensor responses can be exercised on the host.
class SensorBus {
public:
    virtual ~SensorBus() = default;
    virtual bool command(uint8_t address, uint8_t value) = 0;
    virtual void waitMs(uint32_t milliseconds) = 0;
    virtual size_t read(uint8_t address, uint8_t* bytes, size_t capacity) = 0;
};
// High-repeatability measurement, heater disabled. Both outputs are NaN on any failure.
// SHT4x datasheet: 0xFD, 10 ms conversion, two 16-bit words with separate CRC-8 checks.
bool readSht4x(SensorBus& bus, uint8_t address, float& temperature, float& humidity);
} // namespace cajui
