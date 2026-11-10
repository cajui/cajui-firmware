// SPDX-License-Identifier: Apache-2.0
// A complete transmitter profile without any sensor pin: profile.h must reject it.
#pragma once
#include <cstdint>

namespace board {
constexpr const char* BoardName = "heltec_wireless_stick_lite_v3";
constexpr uint8_t Vext = 36, Led = 35;
constexpr uint8_t RadioCs = 8, RadioClock = 9, RadioMiso = 11, RadioMosi = 10;
constexpr uint8_t RadioReset = 12, RadioBusy = 13, RadioDio = 14, RadioRfSwitch = NoPin;
constexpr uint8_t PairButton = 0; // PRG.
constexpr bool PairButtonExternalPullup = false, HasDisplay = false, HasBatterySense = true;
constexpr uint8_t BatterySense = 1, BatteryControl = 37;
constexpr uint8_t OledSda = NoPin, OledScl = NoPin, OledReset = NoPin;
constexpr uint8_t SensorSda = NoPin, SensorScl = NoPin, SensorData = NoPin, SensorAnalog = NoPin;
} // namespace board
