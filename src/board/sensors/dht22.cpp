// SPDX-License-Identifier: Apache-2.0
// DHT22/AM2302 on one data pin, behind the board's switched sensor supply where it has one.
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1 && defined(CAJUI_SENSOR_DHT22)
#include "board/climate.h"
#include "board/common.h"
#include "board/power.h"
#include "board/profile.h"
#include <Arduino.h>
#include <DHT.h>
#include <limits>

namespace board {
void climateOff() {
    pinMode(SensorData, INPUT);
    sensorPower(false);
}
void readClimate(float& temperature, float& humidity) {
    temperature = humidity = std::numeric_limits<float>::quiet_NaN();
    sensorPower(true);
    if (HasDisplay) output(OledReset, LOW);
    DHT sensor(SensorData, DHT22);
    sensor.begin();
    constexpr uint32_t DhtWarmupMs = 2200;
    delay(DhtWarmupMs);
    humidity = sensor.readHumidity();
    temperature = sensor.readTemperature();
    climateOff();
}
} // namespace board
#endif
