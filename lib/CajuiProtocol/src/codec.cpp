#include "protocol_validation.h"
#include <cstring>

namespace cajui {
namespace {
void put(uint8_t* out, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) out[size - 1 - i] = uint8_t(value >> (i * 8));
}
uint64_t get(const uint8_t* in, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value = (value << 8) | in[i];
    return value;
}
void nonceFor(Type type, uint64_t counter, uint8_t nonce[12]) {
    nonce[0] = 'C'; nonce[1] = 'J'; nonce[2] = uint8_t(type); nonce[3] = 1;
    put(nonce + 4, counter, 8);
}
}

namespace detail {
bool validData(const Data& data) {
    if (!data.count || data.count > MaxReadings || !data.nextSeconds) return false;
    for (size_t i = 0; i < data.count; ++i) {
        const auto& r = data.readings[i];
        if (!r.sensor || !r.metric || !r.unit || uint8_t(r.status) > 2 ||
            (r.status != Status::Ok && r.milliValue != 0)) return false;
        for (size_t j = 0; j < i; ++j)
            if (r.sensor == data.readings[j].sensor && r.metric == data.readings[j].metric)
                return false;
    }
    return true;
}
bool usable(const Binding& b) { return b.active && b.network && b.node; }
} // namespace detail

Result seal(const Binding& b, const Message& m, Frame& out) {
    out = Frame{};
    if (!detail::usable(b)) return Result::Unauthorized;
    if (!m.counter || (m.type != Type::Data && m.type != Type::Ack)) return Result::Invalid;
    uint8_t plain[MaxPayload]{};
    size_t length = TagSize;
    if (m.type == Type::Data) {
        if (!detail::validData(m.data)) return Result::Invalid;
        length = 7 + size_t(m.data.count) * 10;
        put(plain, m.data.batteryMv, 2);
        put(plain + 2, m.data.nextSeconds, 4);
        plain[6] = m.data.count;
        for (size_t i = 0; i < m.data.count; ++i) {
            const auto& r = m.data.readings[i];
            uint8_t* p = plain + 7 + i * 10;
            put(p, r.sensor, 2); put(p + 2, r.metric, 2);
            p[4] = r.unit; p[5] = uint8_t(r.status);
            put(p + 6, static_cast<uint32_t>(r.milliValue), 4);
        }
    } else {
        std::memcpy(plain, m.dataTag.data(), TagSize);
    }
    Frame frame{};
    auto* h = frame.bytes.data();
    std::memcpy(h, "CJLR", 4); h[4] = 1; h[5] = uint8_t(m.type);
    put(h + 6, b.network, 8); put(h + 14, b.node, 8);
    put(h + 22, m.counter, 8); put(h + 30, length, 2);
    uint8_t nonce[12]; nonceFor(m.type, m.counter, nonce);
    if (!encrypt(b.key, nonce, h, HeaderSize, plain, length,
                 h + HeaderSize, h + HeaderSize + length)) return Result::CryptoError;
    frame.size = HeaderSize + length + TagSize;
    out = frame;
    return Result::Ok;
}

Result open(const Binding& b, const Frame& frame, Message& out) {
    out = Message{};
    if (!detail::usable(b)) return Result::Unauthorized;
    if (frame.size < HeaderSize + TagSize || frame.size > MaxFrame) return Result::Invalid;
    const auto* h = frame.bytes.data();
    if (std::memcmp(h, "CJLR", 4) || h[4] != 1 || (h[5] != 1 && h[5] != 2))
        return Result::Invalid;
    if (get(h + 6, 8) != b.network || get(h + 14, 8) != b.node) return Result::Unauthorized;
    const auto type = static_cast<Type>(h[5]);
    const uint64_t counter = get(h + 22, 8);
    const size_t length = size_t(get(h + 30, 2));
    if (!counter || length > MaxPayload || frame.size != HeaderSize + length + TagSize ||
        (type == Type::Ack && length != TagSize) ||
        (type == Type::Data && (length < 17 || (length - 7) % 10))) return Result::Invalid;
    uint8_t plain[MaxPayload]{}, nonce[12]; nonceFor(type, counter, nonce);
    if (!decrypt(b.key, nonce, h, HeaderSize, h + HeaderSize, length,
                 h + HeaderSize + length, plain)) return Result::CryptoError;
    Message decoded{};
    decoded.type = type; decoded.counter = counter;
    if (type == Type::Ack) {
        std::memcpy(decoded.dataTag.data(), plain, TagSize);
    } else {
        auto& d = decoded.data;
        d.batteryMv = uint16_t(get(plain, 2)); d.nextSeconds = uint32_t(get(plain + 2, 4));
        d.count = plain[6];
        if (d.count > MaxReadings || length != 7 + size_t(d.count) * 10) return Result::Invalid;
        for (size_t i = 0; i < d.count; ++i) {
            const uint8_t* p = plain + 7 + i * 10;
            auto& r = d.readings[i];
            r.sensor = uint16_t(get(p, 2)); r.metric = uint16_t(get(p + 2, 2));
            r.unit = p[4]; r.status = static_cast<Status>(p[5]);
            const uint32_t value = uint32_t(get(p + 6, 4));
            // Defined conversion for signed two's-complement values on the wire.
            r.milliValue = value <= uint32_t(INT32_MAX) ? int32_t(value) :
                          int32_t(int64_t(value) - INT64_C(4294967296));
        }
        if (!detail::validData(d)) return Result::Invalid;
    }
    out = decoded;
    return Result::Ok;
}
bool sameFrame(const Frame& a, const Frame& b) {
    return a.size <= MaxFrame && a.size == b.size &&
           std::memcmp(a.bytes.data(), b.bytes.data(), a.size) == 0;
}
} // namespace cajui
