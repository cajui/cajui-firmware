// SPDX-License-Identifier: Apache-2.0
// One analog input, reported as its voltage. What the voltage means (soil moisture, light,
// a level) belongs to the consumer, which can then be recalibrated without new firmware.
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "board/sensors/sensors.h"
#include "board/power.h"
#include "board/profile.h"
#include "cajui_registry.h"
#include <Arduino.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>

namespace board {
namespace {
constexpr uint32_t SupplySettleMs = 100, PullSettleMs = 10, RecoverMs = 50;
constexpr int Samples = 16;
constexpr float MilliPerVolt = 1000, MaxVolts = 3.3f;
// Raw 12-bit reading with the internal pull-down on. Nothing identifies an analog source,
// so presence is inferred: the pull-down takes an unconnected pin to ground, while an output
// that drives the pin stays above this. On a XIAO ESP32-S3 a capacitive soil moisture
// sensor read 2.69 V in air and 1.04 V in water and was detected (2026-11-10); the
// unconnected case has not been measured. A source that legitimately rests near 0 V is not
// detected and needs another way to be declared.
constexpr int DrivenRaw = 200;
const cajui::SensorInfo Info = {
    "analog", AnalogSensorId, 1, {{cajui::metric::Voltage, 0, MaxVolts}}};
bool driven() {
    const int8_t channel = digitalPinToAnalogChannel(SensorAnalog);
    if (channel < 0) return false;
    // Configured first: setting up an ADC channel releases the pad's pulls. The raw read
    // below does not repeat that setup, unlike analogRead(), so the pull-down stays on.
    analogSetPinAttenuation(SensorAnalog, ADC_11db);
    rtc_gpio_pulldown_en(gpio_num_t(SensorAnalog));
    delay(PullSettleMs);
    const int raw = adc1_get_raw(adc1_channel_t(channel));
    rtc_gpio_pulldown_dis(gpio_num_t(SensorAnalog));
    return raw >= DrivenRaw;
}
class AnalogInput final : public cajui::Sensor {
public:
    const cajui::SensorInfo& info() const override { return Info; }
    // The presence test runs on every read, not only when probing: an unconnected pin
    // floats, and its reading would otherwise travel as a valid voltage after a cable fails.
    bool read(float* values, bool) override {
        sensorPower(true);
        if (Vext != NoPin) delay(SupplySettleMs);
        const bool answered = driven();
        if (answered) {
            // The pull-down loaded the source; let its output return before measuring.
            delay(RecoverMs);
            analogSetPinAttenuation(SensorAnalog, ADC_11db);
            uint32_t sum = 0;
            for (int i = 0; i < Samples; ++i) sum += analogReadMilliVolts(SensorAnalog);
            values[0] = float(sum) / Samples / MilliPerVolt;
        }
        off();
        return answered;
    }
    void off() override { sensorPower(false); }
};
} // namespace
cajui::Sensor& analogSensor() {
    static AnalogInput sensor;
    return sensor;
}
} // namespace board
#endif
