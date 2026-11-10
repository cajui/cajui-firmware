// SPDX-License-Identifier: Apache-2.0
// Sensirion SHT4x over I2C, behind the board's switched sensor supply where it has one. The
// part answers with a CRC on each word, so a valid answer also tells that it is attached.
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "board/sensors/sensors.h"
#include "board/power.h"
#include "board/profile.h"
#include "cajui_registry.h"
#include "cajui_sht4x.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

namespace board {
namespace {
constexpr uint32_t SupplySettleMs = 100, BusFrequencyHz = 100000;
constexpr uint16_t BusTimeoutMs = 50;
const cajui::SensorInfo Info = {"sht4x",
                                Sht4xSensorId,
                                2,
                                {{cajui::metric::Temperature, MinClimateCelsius, MaxClimateCelsius},
                                 {cajui::metric::Humidity, 0, MaxHumidityPercent}}};
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
class Sht4x final : public cajui::Sensor {
public:
    const cajui::SensorInfo& info() const override { return Info; }
    // Reads once and releases the bus, then switches sensor power off where available.
    bool read(float* values, bool) override {
        sensorPower(true);
        if (Vext != NoPin) delay(SupplySettleMs); // Allow the switched supply to settle.
        started_ = Wire.begin(SensorSda, SensorScl, BusFrequencyHz);
        bool answered = false;
        if (started_) {
            Wire.setTimeOut(BusTimeoutMs);
            WireSensorBus bus;
            answered = cajui::readSht4x(bus, SensorAddress, values[0], values[1]);
        }
        off();
        return answered;
    }
    void off() override {
        if (started_) Wire.end();
        started_ = false;
        floating(SensorSda);
        floating(SensorScl);
        sensorPower(false);
    }

private:
    bool started_ = false;
};
} // namespace
cajui::Sensor& sht4xSensor() {
    static Sht4x sensor;
    return sensor;
}
} // namespace board
#endif
