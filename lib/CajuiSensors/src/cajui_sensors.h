// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_protocol.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
constexpr size_t MaxSensors = 8, MaxChannels = 4;
// A sensor that stops answering this many samples in a row makes the next sample look for
// sensors again, in case another one took its place.
constexpr uint8_t RedetectAfter = 3;
// One quantity a sensor measures: a registry metric and the range its part can report. A
// value outside the range, or not finite, is an error reading and never a clamped one.
struct Channel {
    uint16_t metric;
    float minimum, maximum;
};
// `id` is the sensor ID on the wire. It is fixed per driver, so a sensor keeps its identity
// across restarts and a transmitter can carry several sensors of the same metric.
struct SensorInfo {
    const char* name;
    uint16_t id;
    uint8_t count;
    Channel channels[MaxChannels];
};
class Sensor {
public:
    virtual ~Sensor() = default;
    virtual const SensorInfo& info() const = 0;
    // Reads every channel into values[0..count). False: nothing answered. `probing` is true
    // while the sensor is not known to be attached; a driver whose part cannot identify
    // itself must then answer only when it is sure that something is connected.
    virtual bool read(float* values, bool probing) = 0;
    // Leaves the sensor's pins and supply in their sleep state. Safe before any read.
    virtual void off() = 0;
};
// The sensors that answered, kept across deep sleep. Plain data without initializers, so it
// can live in RTC memory; reset() gives the state after power-on.
struct Detection {
    uint8_t present; // Bit i: sensors[i] answered since the last reset.
    uint8_t misses[MaxSensors];
};
void reset(Detection&);
// Reads one sample. `sensors` lists every driver the board has pins for, always in the same
// order. With no sensor known, or after one missed RedetectAfter samples in a row, every
// driver is tried and those that answer become known. A known sensor is read on every
// sample and reported as error readings while it does not answer: only reset() forgets it,
// so a broken sensor never disappears silently. With nothing known, the first driver's
// channels are reported as errors, since a sample needs a reading and the fault must show.
Data collectSample(Sensor* const* sensors, size_t count, Detection& state, uint32_t nextSeconds,
                   uint16_t batteryMv);
} // namespace cajui
