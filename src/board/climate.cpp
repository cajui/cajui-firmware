// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "climate.h"
#include "profile.h"
#include "common.h"
#include "display.h"
#include "sx1262_radio.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <limits>
#ifdef CAJUI_BOARD_STICK_LITE
#include <Wire.h>
#include "cajui_sht4x.h"
#else
#include <DHT.h>
#endif

namespace board {
namespace {
#ifdef CAJUI_BOARD_STICK_LITE
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
#endif
} // namespace
void climateOff() {
#ifdef CAJUI_BOARD_STICK_LITE
    if (busStarted) Wire.end();
    busStarted = false;
    floating(SensorSda);
    floating(SensorScl);
#else
    pinMode(SensorData, INPUT);
#endif
    output(Vext, HIGH);
}
void readClimate(float& temperature, float& humidity) {
    temperature = humidity = std::numeric_limits<float>::quiet_NaN();
    output(Vext, LOW);
#ifdef CAJUI_BOARD_STICK_LITE
    delay(SupplySettleMs); // Allow the external breakout's supply to settle after Vext rises.
    busStarted = Wire.begin(SensorSda, SensorScl, BusFrequencyHz);
    if (busStarted) {
        Wire.setTimeOut(BusTimeoutMs);
        WireSensorBus bus;
        cajui::readSht4x(bus, SensorAddress, temperature, humidity);
    }
#else
    if (HasDisplay) output(OledReset, LOW);
    DHT sensor(SensorData, DHT22);
    sensor.begin();
    constexpr uint32_t DhtWarmupMs = 2200;
    delay(DhtWarmupMs);
    humidity = sensor.readHumidity();
    temperature = sensor.readTemperature();
#endif
    climateOff();
}
} // namespace board
#endif
