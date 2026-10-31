// SPDX-License-Identifier: Apache-2.0
#include <Arduino.h>
#include <driver/gpio.h>
#include <array>
#include <cassert>
#include "board/profile.h"
#include "board/power.h"
#include "board/battery.h"

namespace {
struct Pin {
    int mode = INPUT, level = LOW, physical = LOW;
    bool held = false, touched = false;
};
std::array<Pin, 49> pins;
unsigned calls = 0;
bool sleeping = false;
uint32_t adc = 750;
int batteryIdle = LOW;
Pin& get(int pin) {
    assert(pin >= 0 && pin < int(pins.size()));
    auto& result = pins[size_t(pin)];
    result.touched = true;
    ++calls;
    return result;
}
void expectHeld(uint8_t pin, int level) {
    if (pin == board::NoPin) return;
    const auto& state = pins[pin];
    assert(state.held && state.mode == OUTPUT && state.level == level);
    assert(state.physical == level);
}
}
void digitalWrite(uint8_t pin, uint8_t level) {
    auto& state = get(pin);
    state.level = level;
    if (!state.held) state.physical = level;
}
void pinMode(uint8_t pin, uint8_t mode) {
    get(pin).mode = mode;
}
int digitalRead(uint8_t pin) {
    get(pin);
    return batteryIdle;
}
void delay(uint32_t) {
    ++calls;
}
void analogSetPinAttenuation(uint8_t pin, uint8_t) {
    get(pin);
}
uint32_t analogReadMilliVolts(uint8_t pin) {
    get(pin);
    return adc;
}
void gpio_hold_en(gpio_num_t pin) {
    get(pin).held = true;
}
void gpio_hold_dis(gpio_num_t pin) {
    auto& state = get(pin);
    state.held = false;
    state.physical = state.level;
}
void gpio_deep_sleep_hold_en() {
    sleeping = true;
}
void gpio_deep_sleep_hold_dis() {
    sleeping = false;
}

int main() {
    board::initializePowerPins();
    assert(pins[board::RadioCs].level == HIGH);
    assert(pins[board::RadioReset].level == LOW);
    board::sensorPower(true);
    if (board::Vext != board::NoPin) assert(pins[board::Vext].level == LOW);
    board::sensorPower(false);
    if (board::Vext != board::NoPin) assert(pins[board::Vext].level == HIGH);
    for (bool radioSleeping : {true, false}) {
        board::prepareSleepPins(radioSleeping);
        assert(sleeping);
        expectHeld(board::RadioCs, HIGH);
        expectHeld(board::RadioReset, radioSleeping ? HIGH : LOW);
        expectHeld(board::Vext, HIGH);
        expectHeld(board::Led, LOW);
        expectHeld(board::RadioRfSwitch, LOW);
        board::initializePowerPins();
        assert(!sleeping);
        for (const auto& pin : pins) assert(!pin.held);
        assert(pins[board::RadioCs].physical == HIGH);
        assert(pins[board::RadioReset].physical == LOW);
    }
    if (!board::HasBatterySense) {
        const auto before = calls;
        assert(board::readBatteryMv() == 0);
        assert(before == calls);
        for (uint8_t pin : {1, 21, 33, 34, 35, 36, 37}) assert(!pins[pin].touched);
    } else {
        for (int idle : {LOW, HIGH}) {
            batteryIdle = idle;
            assert(board::readBatteryMv() > 3800 && board::readBatteryMv() < 3900);
            assert(pins[board::BatteryControl].mode == ANALOG);
            assert(pins[board::BatteryControl].level == (idle ? LOW : HIGH));
        }
        adc = 0;
        assert(board::readBatteryMv() == 0);
        adc = 1000;
        assert(board::readBatteryMv() == 0);
    }
}
