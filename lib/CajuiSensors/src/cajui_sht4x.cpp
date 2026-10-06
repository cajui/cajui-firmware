// SPDX-License-Identifier: Apache-2.0
#include "cajui_sht4x.h"
#include <limits>

namespace cajui {
namespace {
constexpr uint8_t CrcInitial = 0xff, CrcPolynomial = 0x31, FirstAddress = 0x44, LastAddress = 0x46,
                  Measure = 0xfd;
constexpr uint32_t ConversionMs = 10;
constexpr float TemperatureOffset = -45.0f, TemperatureScale = 175.0f, HumidityOffset = -6.0f,
                HumidityScale = 125.0f, FullScale = 65535.0f, MaxHumidity = 100.0f;
constexpr size_t ResponseBytes = 6, HumidityOffsetBytes = 3, HumidityChecksumByte = 5;
uint8_t checksum(const uint8_t* word) {
    uint8_t crc = CrcInitial;
    for (size_t i = 0; i < 2; ++i) {
        crc ^= word[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = uint8_t((crc & 0x80) ? (crc << 1) ^ CrcPolynomial : crc << 1);
    }
    return crc;
}
uint16_t ticks(const uint8_t* word) {
    return uint16_t(uint16_t(word[0]) << 8 | word[1]);
}
} // namespace
bool readSht4x(SensorBus& bus, uint8_t address, float& temperature, float& humidity) {
    temperature = humidity = std::numeric_limits<float>::quiet_NaN();
    if (address < FirstAddress || address > LastAddress || !bus.command(address, Measure))
        return false;
    bus.waitMs(ConversionMs);
    uint8_t bytes[ResponseBytes]{};
    if (bus.read(address, bytes, sizeof(bytes)) != sizeof(bytes)) return false;
    if (checksum(bytes) != bytes[2] ||
        checksum(bytes + HumidityOffsetBytes) != bytes[HumidityChecksumByte])
        return false;
    temperature = TemperatureOffset + TemperatureScale * float(ticks(bytes)) / FullScale;
    humidity =
        HumidityOffset + HumidityScale * float(ticks(bytes + HumidityOffsetBytes)) / FullScale;
    if (humidity < 0) humidity = 0;
    if (humidity > MaxHumidity) humidity = MaxHumidity;
    return true;
}
} // namespace cajui
