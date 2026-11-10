// SPDX-License-Identifier: Apache-2.0
// DHT22/AM2302 on one data pin, behind the board's switched sensor supply where it has one.
// The part cannot identify itself, but it answers a start signal with 40 bits and a
// checksum: an idle or floating pin never does, so a valid answer tells that it is attached.
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "board/sensors/sensors.h"
#include "board/common.h"
#include "board/power.h"
#include "board/profile.h"
#include "cajui_registry.h"
#include <Arduino.h>
#include <DHT.h>
#include <cmath>

namespace board {
namespace {
constexpr uint32_t DhtWarmupMs = 2200;
const cajui::SensorInfo Info = {"dht22",
                                Dht22SensorId,
                                2,
                                {{cajui::metric::Temperature, MinClimateCelsius, MaxClimateCelsius},
                                 {cajui::metric::Humidity, 0, MaxHumidityPercent}}};
class Dht22 final : public cajui::Sensor {
public:
    const cajui::SensorInfo& info() const override { return Info; }
    bool read(float* values, bool) override {
        sensorPower(true);
        if (HasDisplay) output(OledReset, LOW);
        DHT sensor(SensorData, DHT22);
        sensor.begin();
        delay(DhtWarmupMs);
        values[1] = sensor.readHumidity();
        values[0] = sensor.readTemperature();
        off();
        return std::isfinite(values[0]) && std::isfinite(values[1]);
    }
    void off() override {
        pinMode(SensorData, INPUT);
        sensorPower(false);
    }
};
} // namespace
cajui::Sensor& dht22Sensor() {
    static Dht22 sensor;
    return sensor;
}
} // namespace board
#endif
