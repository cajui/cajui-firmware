// SPDX-License-Identifier: Apache-2.0
// Replaces the Adafruit DHT library, whose read keeps interrupts off while it waits for all
// 80 pulses even after one times out, and bounds each wait by loop iterations rather than
// time: at 80 MHz a line held at one level kept interrupts off for seconds, and the
// interrupt watchdog restarted the chip at every detection pass (XIAO ESP32-S3, 2026-11-10).
#include "cajui_dht22.h"
#include <limits>

namespace cajui {
namespace {
constexpr size_t FrameBytes = 5, FrameBits = FrameBytes * 8;
constexpr uint8_t SignBit = 0x80, MagnitudeMask = 0x7f;
constexpr float Tenths = 10.0f;
// The line rises within a few microseconds once the start signal is released.
constexpr uint32_t ReleaseMicros = 50;
bool timedOut(uint32_t micros, uint32_t timeout) {
    return micros > timeout;
}
} // namespace
bool decodeDht22(const uint8_t frame[FrameBytes], float& temperature, float& humidity) {
    temperature = humidity = std::numeric_limits<float>::quiet_NaN();
    const uint8_t sum = uint8_t(frame[0] + frame[1] + frame[2] + frame[3]);
    if (sum != frame[4]) return false;
    // Forty highs shorter than their lows decode to zeros, and zeros pass the checksum: a
    // line toggling with no part on it. 0 %RH at 0 degrees is that, not a measurement.
    if (!frame[0] && !frame[1] && !frame[2] && !frame[3]) return false;
    humidity = float(unsigned(frame[0]) << 8u | frame[1]) / Tenths;
    temperature = float(unsigned(frame[2] & MagnitudeMask) << 8u | frame[3]) / Tenths;
    if (frame[2] & SignBit) temperature = -temperature;
    return true;
}
bool readDht22(Dht22Line& line, float& temperature, float& humidity) {
    temperature = humidity = std::numeric_limits<float>::quiet_NaN();
    const uint32_t timeout = Dht22LevelTimeoutMicros;
    // The end of the start signal, the gap before the part answers, then its answer.
    if (timedOut(line.hold(false, ReleaseMicros), ReleaseMicros) ||
        timedOut(line.hold(true, timeout), timeout) ||
        timedOut(line.hold(false, timeout), timeout) || timedOut(line.hold(true, timeout), timeout))
        return false;
    uint8_t frame[FrameBytes]{};
    for (size_t bit = 0; bit < FrameBits; ++bit) {
        const uint32_t low = line.hold(false, timeout);
        if (timedOut(low, timeout)) return false;
        const uint32_t high = line.hold(true, timeout);
        if (timedOut(high, timeout)) return false;
        // A one's high outlasts the ~50 us low that precedes it; a zero's does not.
        frame[bit / 8] = uint8_t(unsigned(frame[bit / 8]) << 1u | (high > low ? 1u : 0u));
    }
    return decodeDht22(frame, temperature, humidity);
}
} // namespace cajui
