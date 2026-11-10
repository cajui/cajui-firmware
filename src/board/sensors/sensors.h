// SPDX-License-Identifier: Apache-2.0
// The transmitter's sensor drivers. Every driver is built into every transmitter image; the
// board profile decides which ones have pins, and the transmitter finds out at run time
// which of those are attached (docs/adr/0002-sensors-detected-at-run-time.md).
#pragma once
#include "cajui_sensors.h"
#include <cstddef>
#include <cstdint>

namespace board {
// Sensor IDs on the wire, one per driver. Append-only, like the metric registry: an ID in
// use never moves to another driver, or a sensor would change identity after an update.
constexpr uint16_t Sht4xSensorId = 1, Dht22SensorId = 2, AnalogSensorId = 3;
constexpr float MinClimateCelsius = -40, MaxClimateCelsius = 80, MaxHumidityPercent = 100;
// One instance per driver file.
cajui::Sensor& sht4xSensor();
cajui::Sensor& dht22Sensor();
cajui::Sensor& analogSensor();
// The drivers this board has pins for, always in the same order.
size_t sensorDrivers(cajui::Sensor** drivers, size_t capacity);
// Every driver's pins and supply in their sleep state. Safe before any read and before any
// deep sleep (also the critical-battery path).
void sensorsOff();
} // namespace board
