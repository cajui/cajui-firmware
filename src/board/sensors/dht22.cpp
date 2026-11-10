// SPDX-License-Identifier: Apache-2.0
// DHT22/AM2302 on one data pin, behind the board's switched sensor supply where it has one.
// The part cannot identify itself, but it answers a start signal with 40 bits and a
// checksum: an idle or floating pin never does, so a valid answer tells that it is attached.
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "board/sensors/sensors.h"
#include "board/common.h"
#include "board/power.h"
#include "board/profile.h"
#include "cajui_dht22.h"
#include "cajui_registry.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace board {
namespace {
// The part needs two seconds after power-on before it answers; the datasheet asks for at
// least 1 ms of start signal.
constexpr uint32_t DhtWarmupMs = 2200, StartSignalMicros = 1100;
const cajui::SensorInfo Info = {"dht22",
                                Dht22SensorId,
                                2,
                                {{cajui::metric::Temperature, MinClimateCelsius, MaxClimateCelsius},
                                 {cajui::metric::Humidity, 0, MaxHumidityPercent}}};
class Line final : public cajui::Dht22Line {
public:
    uint32_t hold(bool high, uint32_t timeoutMicros) override {
        const int64_t start = esp_timer_get_time();
        while ((gpio_get_level(gpio_num_t(SensorData)) != 0) == high)
            if (esp_timer_get_time() - start > int64_t(timeoutMicros)) return timeoutMicros + 1;
        return uint32_t(esp_timer_get_time() - start);
    }
};
portMUX_TYPE timing = portMUX_INITIALIZER_UNLOCKED;
class Dht22 final : public cajui::Sensor {
public:
    const cajui::SensorInfo& info() const override { return Info; }
    bool read(float* values, bool) override {
        sensorPower(true);
        if (HasDisplay) output(OledReset, LOW);
        pinMode(SensorData, INPUT_PULLUP);
        delay(DhtWarmupMs);
        bool answered = false;
        // An idle DHT22 holds the line high. Low here is something else on the pin.
        if (digitalRead(SensorData) == HIGH) {
            // Open drain with the pull-up: releasing the line is one register write, safe
            // inside the critical section, where pinMode() is not.
            const auto pin = gpio_num_t(SensorData);
            gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
            gpio_set_level(pin, 0);
            delayMicroseconds(StartSignalMicros);
            Line line;
            // Bounded by readDht22: at most 84 waits of 120 us, well under the interrupt
            // watchdog's timeout.
            portENTER_CRITICAL(&timing);
            gpio_set_level(pin, 1);
            answered = cajui::readDht22(line, values[0], values[1]);
            portEXIT_CRITICAL(&timing);
        }
        off();
        return answered;
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
