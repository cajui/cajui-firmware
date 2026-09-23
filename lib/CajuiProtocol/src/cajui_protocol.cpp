#include "cajui_protocol.h"
#include <cstring>
#include <limits>

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
bool valid(const Data& data) {
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
void nonceFor(Type type, uint64_t counter, uint8_t nonce[12]) {
    nonce[0] = 'C'; nonce[1] = 'J'; nonce[2] = uint8_t(type); nonce[3] = 1;
    put(nonce + 4, counter, 8);
}
bool usable(const Binding& b) { return b.active && b.network && b.node; }
}

Result seal(const Binding& b, const Message& m, Frame& out) {
    out = Frame{};
    if (!usable(b)) return Result::Unauthorized;
    if (!m.counter || (m.type != Type::Data && m.type != Type::Ack)) return Result::Invalid;
    uint8_t plain[MaxPayload]{};
    size_t length = TagSize;
    if (m.type == Type::Data) {
        if (!valid(m.data)) return Result::Invalid;
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
    if (!usable(b)) return Result::Unauthorized;
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
        if (!valid(d)) return Result::Invalid;
    }
    out = decoded;
    return Result::Ok;
}
bool sameFrame(const Frame& a, const Frame& b) {
    return a.size <= MaxFrame && a.size == b.size &&
           std::memcmp(a.bytes.data(), b.bytes.data(), a.size) == 0;
}
Result receive(const Binding& b, const Frame& frame, Journal& journal, Frame& ack) {
    ack = Frame{};
    Message m{};
    const auto decoded = open(b, frame, m);
    if (decoded != Result::Ok) return decoded;
    if (m.type != Type::Data) return Result::Invalid;
    Receipt old{};
    if (!journal.load(b, old)) return Result::StorageError;
    if (m.counter < old.counter) return Result::Replay;
    const bool duplicate = m.counter == old.counter;
    if (duplicate && !sameFrame(old.last, frame)) return Result::Conflict;
    if (!duplicate) {
        Receipt next{}; next.counter = m.counter; next.last = frame;
        const auto saved = journal.commit(b, old.counter, next, m.data);
        if (saved != Result::Ok) return saved;
    }
    Message response{};
    response.type = Type::Ack; response.counter = m.counter;
    std::memcpy(response.dataTag.data(), frame.bytes.data() + frame.size - TagSize, TagSize);
    const auto sealed = seal(b, response, ack);
    if (sealed != Result::Ok) return sealed;
    return duplicate ? Result::Duplicate : Result::Ok;
}
Result Sender::begin(const Binding& b, const Data& data, CounterStore& store) {
    if (pending_.size && !delivered_) return Result::Conflict;
    if (!usable(b)) return Result::Unauthorized;
    if (!valid(data)) return Result::Invalid;
    uint64_t counter = 0;
    if (!store.reserve(b, counter) || !counter) return Result::StorageError;
    Message m{}; m.counter = counter; m.data = data;
    Frame next{};
    const auto result = seal(b, m, next);
    if (result != Result::Ok) return result;
    binding_ = b; pending_ = next; counter_ = counter; attempts_ = 0; delivered_ = false;
    return Result::Ok;
}
void Sender::abandon() {
    pending_ = Frame{}; counter_ = 0; attempts_ = 0; delivered_ = false;
}
const Frame* Sender::nextAttempt() {
    if (!pending_.size || delivered_ || attempts_ >= MaxAttempts) return nullptr;
    ++attempts_;
    return &pending_;
}
Result Sender::acknowledge(const Frame& ack) {
    if (!pending_.size || !attempts_) return Result::Invalid;
    Message m{};
    const auto result = open(binding_, ack, m);
    if (result != Result::Ok) return result;
    if (m.type != Type::Ack || m.counter != counter_ ||
        std::memcmp(m.dataTag.data(), pending_.bytes.data() + pending_.size - TagSize, TagSize))
        return Result::Invalid;
    delivered_ = true;
    return Result::Ok;
}
} // namespace cajui
