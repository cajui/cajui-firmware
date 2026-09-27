// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "battery.h"
#include <Arduino.h>
#include <cmath>

namespace board {
namespace {
// Heltec WiFi LoRa 32 V3: VBAT - 390 kOhm - GPIO1 - 100 kOhm - GND, connected only while
// GPIO37 enables it (github.com/ropg/heltec_esp32_lora_v3, README). The enabling level
// differs between board revisions; like Meshtastic (src/Power.cpp, variants/heltec_v3), the
// line's idle level, set by its external resistor, is read and the opposite driven, then the
// line returns to high impedance. The 1.045 correction is Meshtastic's, not measured here.
constexpr uint8_t BatterySense = 1, BatteryControl = 37;
constexpr float Divider = 4.9f * 1.045f;
constexpr int Samples = 16;
constexpr uint32_t SettleMs = 10;
constexpr float MinCellMv = 2500, MaxCellMv = 4500;
}
uint16_t readBatteryMv() {
    pinMode(BatteryControl, INPUT);
    const int idle = digitalRead(BatteryControl);
    pinMode(BatteryControl, OUTPUT);
    digitalWrite(BatteryControl, idle ? LOW : HIGH);
    delay(SettleMs);
    // Low attenuation suits the divider's high impedance; 4.5 V divided is well under range.
    analogSetPinAttenuation(BatterySense, ADC_2_5db);
    uint32_t sum = 0;
    for (int i = 0; i < Samples; ++i) sum += analogReadMilliVolts(BatterySense);
    pinMode(BatteryControl, ANALOG);
    const float millivolts = float(sum) / Samples * Divider;
    if (millivolts < MinCellMv || millivolts > MaxCellMv) return 0;
    return uint16_t(lroundf(millivolts));
}
} // namespace board
#endif
