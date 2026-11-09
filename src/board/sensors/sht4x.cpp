// SPDX-License-Identifier: Apache-2.0
// Sensirion SHT4x over I2C, behind the board's switched sensor supply where it has one.
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1 && defined(CAJUI_SENSOR_SHT4X)
#include "board/climate.h"
#include "board/power.h"
#include "board/profile.h"
#include "cajui_sht4x.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <limits>

namespace board {
namespace {
constexpr uint32_t SupplySettleMs = 100, BusFrequencyHz = 100000;
constexpr uint16_t BusTimeoutMs = 50;
bool busStarted = false;
class WireSensorBus final : public cajui::SensorBus {
public:
    bool command(uint8_t address, uint8_t value) override {
        Wire.beginTransmission(address);
        Wire.write(value);
        return Wire.endTransmission() == 0;
    }
    void waitMs(uint32_t milliseconds) override { delay(milliseconds); }
    size_t read(uint8_t address, uint8_t* bytes, size_t capacity) override {
        const size_t received = Wire.requestFrom(address, capacity);
        size_t count = 0;
        while (Wire.available() && count < capacity) bytes[count++] = uint8_t(Wire.read());
        return received == count ? count : 0;
    }
};
void floating(uint8_t pin) {
    pinMode(pin, INPUT);
    gpio_pullup_dis(gpio_num_t(pin));
    gpio_pulldown_dis(gpio_num_t(pin));
}
} // namespace
void climateOff() {
    if (busStarted) Wire.end();
    busStarted = false;
    floating(SensorSda);
    floating(SensorScl);
    sensorPower(false);
}
void readClimate(float& temperature, float& humidity) {
    temperature = humidity = std::numeric_limits<float>::quiet_NaN();
    sensorPower(true);
    if (Vext != NoPin) delay(SupplySettleMs); // Allow the switched supply to settle.
    busStarted = Wire.begin(SensorSda, SensorScl, BusFrequencyHz);
    if (busStarted) {
        Wire.setTimeOut(BusTimeoutMs);
        WireSensorBus bus;
        cajui::readSht4x(bus, SensorAddress, temperature, humidity);
    }
    climateOff();
}
} // namespace board
#endif
