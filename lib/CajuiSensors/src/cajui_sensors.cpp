// SPDX-License-Identifier: Apache-2.0
#include "cajui_sensors.h"
#include "cajui_registry.h"
#include <cmath>
#include <limits>

namespace cajui {
namespace {
constexpr float MilliScale = 1000;
// Appends the sensor's channels, as measurements when it answered and as errors otherwise.
// The caller checked that they fit. A channel outside the registry has no unit and is left out.
void append(Data& data, const SensorInfo& info, const float* values, bool answered) {
    for (size_t i = 0; i < info.count && i < MaxChannels; ++i) {
        const Channel& channel = info.channels[i];
        const Quantity* known = quantity(channel.metric);
        if (!known) continue;
        Reading reading{};
        reading.sensor = info.id;
        reading.metric = channel.metric;
        reading.unit = known->unit;
        const float value = values[i];
        if (!answered || !std::isfinite(value) || value < channel.minimum ||
            value > channel.maximum)
            reading.status = Status::Error;
        else
            reading.milliValue = int32_t(std::lround(value * MilliScale));
        data.readings[data.count++] = reading;
    }
}
void unknown(float* values) {
    for (size_t i = 0; i < MaxChannels; ++i) values[i] = std::numeric_limits<float>::quiet_NaN();
}
} // namespace
void reset(Detection& state) {
    state = Detection{};
}
Data collectSample(Sensor* const* sensors, size_t count, Detection& state, uint32_t nextSeconds,
                   uint16_t batteryMv) {
    Data data{};
    data.batteryMv = batteryMv;
    data.nextSeconds = nextSeconds;
    if (count > MaxSensors) count = MaxSensors;
    bool detecting = state.present == 0;
    for (size_t i = 0; i < count; ++i)
        if (state.misses[i] >= RedetectAfter) detecting = true;
    float values[MaxChannels];
    for (size_t i = 0; i < count; ++i) {
        const uint8_t bit = uint8_t(1u << i);
        const bool known = (state.present & bit) != 0;
        if (!known && !detecting) continue;
        const SensorInfo& info = sensors[i]->info();
        // A sensor whose channels no longer fit in the frame is not read at all.
        if (data.count + info.count > MaxReadings) continue;
        unknown(values);
        if (sensors[i]->read(values, !known)) {
            state.present = uint8_t(state.present | bit);
            state.misses[i] = 0;
            append(data, info, values, true);
        } else if (known) {
            // The count restarts after a detection pass, so the next one is as far away.
            state.misses[i] = detecting ? uint8_t(0) : uint8_t(state.misses[i] + 1);
            append(data, info, values, false);
        }
    }
    if (!data.count && count) {
        unknown(values);
        append(data, sensors[0]->info(), values, false);
    }
    return data;
}
} // namespace cajui
