// SPDX-License-Identifier: Apache-2.0
// Heltec WiFi LoRa 32 V3: ESP32-S3FN8, SX1262, SSD1306 OLED and a switched battery divider.
// Reached only through profile.h, which defines NoPin and checks the full set of names.
#pragma once
#include <cstdint>

namespace board {
constexpr const char* BoardName = "heltec_wifi_lora_32_v3";
constexpr uint8_t Vext = 36, Led = 35;
constexpr uint8_t RadioCs = 8, RadioClock = 9, RadioMiso = 11, RadioMosi = 10;
constexpr uint8_t RadioReset = 12, RadioBusy = 13, RadioDio = 14, RadioRfSwitch = NoPin;
constexpr uint8_t PairButton = 0; // PRG.
constexpr bool PairButtonExternalPullup = false, HasDisplay = true, HasBatterySense = true;
constexpr uint8_t BatterySense = 1, BatteryControl = 37;
constexpr uint8_t OledSda = 17, OledScl = 18, OledReset = 21;
constexpr uint8_t SensorData = 47, SensorSda = NoPin, SensorScl = NoPin;
} // namespace board
