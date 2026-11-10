// SPDX-License-Identifier: Apache-2.0
// Seeed XIAO ESP32-S3 with the Wio-SX1262 B2B module: no display, no battery divider, no
// switched sensor supply, an RF switch line and an external pull-up on the pairing button.
// Reached only through profile.h.
#pragma once
#include <cstdint>

#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE != 1
#error The XIAO ESP32-S3 profile currently supports transmitters only.
#endif

namespace board {
constexpr const char* BoardName = "xiao_esp32s3_wio_sx1262";
constexpr uint8_t Vext = NoPin, Led = 48;
constexpr uint8_t RadioCs = 41, RadioClock = 7, RadioMiso = 8, RadioMosi = 9;
constexpr uint8_t RadioReset = 42, RadioBusy = 40, RadioDio = 39, RadioRfSwitch = 38;
constexpr uint8_t PairButton = 21;
constexpr bool PairButtonExternalPullup = true, HasDisplay = false, HasBatterySense = false;
constexpr uint8_t BatterySense = NoPin, BatteryControl = NoPin;
constexpr uint8_t OledSda = NoPin, OledScl = NoPin, OledReset = NoPin;
// D4/D5 for I2C, D0 for a one-wire data sensor and D1 (ADC1 channel 1) for an analog one.
constexpr uint8_t SensorSda = 5, SensorScl = 6, SensorData = 1, SensorAnalog = 2;
} // namespace board
