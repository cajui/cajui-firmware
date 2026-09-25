#include "cajui_crc32.h"

namespace cajui {
uint32_t crc32(const uint8_t* data, size_t length) {
    constexpr uint32_t Polynomial = 0xedb88320u;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (Polynomial & (0u - (crc & 1u)));
    }
    return ~crc;
}
} // namespace cajui
