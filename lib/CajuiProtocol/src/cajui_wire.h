#pragma once
#include "cajui_protocol.h"

// Internal helpers shared by the v1 codec and radio pairing; not additional public API.
// Header layout: docs/protocol-v1.md.
namespace cajui {
namespace wire {
constexpr uint8_t Magic[4] = {'C', 'J', 'L', 'R'};
constexpr uint8_t Version = 1;
constexpr size_t VersionAt = 4, TypeAt = 5, NetworkAt = 6, NodeAt = 14, CounterAt = 22,
                 LengthAt = 30;
// Big-endian unsigned integers of `size` bytes.
inline void put(uint8_t* out, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) out[size - 1 - i] = uint8_t(value >> (i * 8));
}
inline uint64_t get(const uint8_t* in, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value = (value << 8) | in[i];
    return value;
}
// GCM nonce: "CJ", frame type, version, then the counter or attempt nonce. The type byte
// keeps DATA, ACK and pairing nonces apart under any key.
inline void nonce(uint8_t type, uint64_t counter, uint8_t out[NonceSize]) {
    out[0] = 'C';
    out[1] = 'J';
    out[2] = type;
    out[3] = Version;
    put(out + 4, counter, 8);
}
} // namespace wire
} // namespace cajui
