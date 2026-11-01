// SPDX-License-Identifier: Apache-2.0
#ifdef CAJUI_RUNTIME_ROLE
#include "power.h"
#include "profile.h"
#include <Arduino.h>
#include <driver/gpio.h>

namespace board {
namespace {
void drive(uint8_t pin, uint8_t level) {
    digitalWrite(pin, level);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
}
void initialize(uint8_t pin, uint8_t level) {
    if (pin == NoPin) return;
    drive(pin, level);
    gpio_hold_dis(gpio_num_t(pin));
    drive(pin, level);
}
void hold(uint8_t pin, uint8_t level) {
    if (pin == NoPin) return;
    drive(pin, level);
    gpio_hold_en(gpio_num_t(pin));
}
} // namespace
void initializePowerPins() {
    initialize(RadioReset, LOW);
    initialize(RadioCs, HIGH);
    initialize(Vext, HIGH);
    initialize(Led, LOW);
    initialize(RadioRfSwitch, LOW);
    gpio_deep_sleep_hold_dis();
}
void sensorPower(bool enabled) {
    if (Vext != NoPin) drive(Vext, enabled ? LOW : HIGH);
}
void prepareSleepPins(bool radioSleeping) {
    hold(RadioReset, radioSleeping ? HIGH : LOW);
    hold(Vext, HIGH);
    hold(Led, LOW);
    hold(RadioCs, HIGH);
    hold(RadioRfSwitch, LOW);
    gpio_deep_sleep_hold_en();
}
} // namespace board
#endif
