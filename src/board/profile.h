// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#if defined(CAJUI_BOARD_XIAO_S3) && defined(CAJUI_BOARD_STICK_LITE)
#error Select exactly one board profile.
#endif
#if defined(CAJUI_SENSOR_SHT4X) && defined(CAJUI_SENSOR_DHT22)
#error Select exactly one climate sensor.
#endif
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1 && !defined(CAJUI_SENSOR_SHT4X) &&      \
    !defined(CAJUI_SENSOR_DHT22)
#error Select a climate sensor for the transmitter.
#endif
#if defined(CAJUI_BOARD_XIAO_S3) && CAJUI_RUNTIME_ROLE != 1
#error The XIAO ESP32-S3 profile currently supports transmitters only.
#endif

namespace board {
constexpr uint8_t NoPin = 255;
#ifdef CAJUI_BOARD_XIAO_S3
constexpr const char* BoardName = "xiao_esp32s3_wio_sx1262";
constexpr uint8_t Vext = NoPin, Led = 48;
constexpr uint8_t RadioCs = 41, RadioClock = 7, RadioMiso = 8, RadioMosi = 9;
constexpr uint8_t RadioReset = 42, RadioBusy = 40, RadioDio = 39, RadioRfSwitch = 38;
constexpr uint8_t PairButton = 21;
constexpr bool PairButtonExternalPullup = true, HasDisplay = false, HasBatterySense = false;
constexpr uint8_t BatterySense = NoPin, BatteryControl = NoPin;
constexpr uint8_t SensorSda = 5, SensorScl = 6, SensorData = 1;
#else
constexpr uint8_t Vext = 36, Led = 35;
constexpr uint8_t RadioCs = 8, RadioClock = 9, RadioMiso = 11, RadioMosi = 10;
constexpr uint8_t RadioReset = 12, RadioBusy = 13, RadioDio = 14, RadioRfSwitch = NoPin;
constexpr uint8_t PairButton = 0;
constexpr bool PairButtonExternalPullup = false, HasBatterySense = true;
constexpr uint8_t BatterySense = 1, BatteryControl = 37;
// The WSL V3 and WiFi LoRa 32 V3 share the ESP32-S3FN8 and SX1262 wiring.
// Sensor wiring and display presence differ; never drive OLED reset on the WSL.
#ifdef CAJUI_BOARD_STICK_LITE
constexpr bool HasDisplay = false;
constexpr uint8_t SensorSda = 33, SensorScl = 34, SensorData = NoPin;
constexpr const char* BoardName = "heltec_wireless_stick_lite_v3";
#else
constexpr uint8_t SensorData = 47, SensorSda = NoPin, SensorScl = NoPin;
constexpr bool HasDisplay = true;
constexpr const char* BoardName = "heltec_wifi_lora_32_v3";
#endif
#endif
constexpr float TcxoVoltage = 1.8f;
constexpr uint8_t SensorAddress = 0x44;
#ifdef CAJUI_SENSOR_SHT4X
static_assert(SensorSda != NoPin && SensorScl != NoPin, "SHT4x requires I2C pins");
constexpr const char* SensorName = "sht4x";
#elif defined(CAJUI_SENSOR_DHT22)
static_assert(SensorData != NoPin, "DHT22 requires a data pin");
constexpr const char* SensorName = "dht22";
#endif
} // namespace board
