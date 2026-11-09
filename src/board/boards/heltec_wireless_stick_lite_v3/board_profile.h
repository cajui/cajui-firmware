// SPDX-License-Identifier: Apache-2.0
// Heltec Wireless Stick Lite V3: the WiFi LoRa 32 V3's ESP32-S3FN8 and SX1262 wiring without
// the display, so the OLED reset is never driven here. Reached only through profile.h.
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
constexpr uint8_t SensorSda = 33, SensorScl = 34, SensorData = NoPin;
} // namespace board
