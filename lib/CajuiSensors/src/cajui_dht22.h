// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace cajui {
// DHT22/AM2302 data line, seen through how long it holds each level. The board adapter
// measures the time; nothing here depends on a clock or a pin.
class Dht22Line {
public:
    virtual ~Dht22Line() = default;
    // Waits while the line is at `high` and returns for how long it stayed, in microseconds.
    // A line that does not change within `timeoutMicros` returns more than that.
    virtual uint32_t hold(bool high, uint32_t timeoutMicros) = 0;
};
// Generous against the datasheet's 85 us maximum for any one level.
constexpr uint32_t Dht22LevelTimeoutMicros = 120;
// Reads one frame after the caller has driven the line low for the start signal and
// released it. The part answers with a low and a high of about 80 us each, then 40 bits,
// each a ~50 us low followed by a high of ~27 us (0) or ~70 us (1). Every wait is bounded
// and the read stops at the first one that times out, so a caller that keeps interrupts off
// while reading does so for at most 84 waits. Both outputs are NaN on any failure.
bool readDht22(Dht22Line&, float& temperature, float& humidity);
// Frame: humidity x10 (u16), temperature x10 (sign bit and magnitude), checksum of the
// first four bytes. False, with NaN outputs, when the checksum does not match or the frame
// is all zeros.
bool decodeDht22(const uint8_t frame[5], float& temperature, float& humidity);
} // namespace cajui
