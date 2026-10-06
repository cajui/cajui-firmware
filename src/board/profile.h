// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace board {
constexpr uint8_t Vext = 36, Led = 35;
// The WSL V3 and WiFi LoRa 32 V3 share the ESP32-S3FN8 and SX1262 wiring.
// Sensor wiring and display presence differ; never drive OLED reset on the WSL.
#ifdef CAJUI_BOARD_STICK_LITE
constexpr bool HasDisplay = false;
constexpr uint8_t SensorSda = 33, SensorScl = 34, SensorAddress = 0x44;
constexpr const char* BoardName = "heltec_wireless_stick_lite_v3";
constexpr const char* SensorName = "sht4x";
#else
constexpr uint8_t SensorData = 47;
constexpr bool HasDisplay = true;
constexpr const char* BoardName = "heltec_wifi_lora_32_v3";
constexpr const char* SensorName = "dht22";
#endif
} // namespace board
