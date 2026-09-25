#pragma once
#include <cstddef>
#include <cstdint>

namespace cajui {
// CRC-32 (reflected IEEE 802.3) for stored blobs. Detects accidental corruption only; it
// is not authentication.
uint32_t crc32(const uint8_t* data, size_t length);
} // namespace cajui
