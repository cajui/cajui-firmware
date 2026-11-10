// SPDX-License-Identifier: Apache-2.0
// A board profile missing OledSda: profile.h must reject it by name.
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
constexpr uint8_t OledScl = NoPin, OledReset = NoPin;
constexpr uint8_t SensorSda = 5, SensorScl = 6, SensorData = 1, SensorAnalog = 2;
} // namespace board
