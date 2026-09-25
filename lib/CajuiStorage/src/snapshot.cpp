#include "snapshot.h"
#include "cajui_crc32.h"
#include <cstring>

namespace cajui {
namespace snapshot {
namespace {
struct Writer {
    uint8_t* p;
    void number(uint64_t n, size_t size) {
        for (size_t i = size; i > 0; --i) *p++ = uint8_t(n >> ((i - 1) * 8));
    }
    void block(const uint8_t* src, size_t size) {
        std::memcpy(p, src, size);
        p += size;
    }
};
struct Reader {
    const uint8_t* p;
    uint64_t number(size_t size) {
        uint64_t n = 0;
        while (size--) n = (n << 8) | *p++;
        return n;
    }
    void block(uint8_t* dst, size_t size) {
        std::memcpy(dst, p, size);
        p += size;
    }
};
constexpr uint8_t SnapshotMagic[4] = {'C', 'J', 'S', 'T'};
constexpr uint8_t SnapshotVersion = 1;
}
bool nonzero(const Key& key) {
    for (auto byte : key)
        if (byte) return true;
    return false;
}
Binding binding(const Entry& e, uint64_t network) {
    Binding b{};
    b.network = network;
    b.node = e.node;
    b.key = e.key;
    b.active = true;
    return b;
}
void reset(State& state) {
    state.revision = state.network = state.receiver = 0;
    state.profile = state.count = 0;
    for (auto& e : state.entries) e = Entry{};
    for (auto& q : state.queue) q = Queued{};
}
size_t encode(const State& state, Role role, uint64_t device, uint8_t* output) {
    Writer w{output};
    w.block(SnapshotMagic, sizeof(SnapshotMagic));
    w.number(SnapshotVersion, 1);
    w.number(uint8_t(role), 1);
    w.number(device, 8);
    w.number(state.revision, 8);
    w.number(state.network, 8);
    w.number(state.receiver, 8);
    w.number(state.profile, 2);
    w.number(state.count, 1); // count <= 128
    // Slots are never freed, so occupied entries form a prefix; queued frames refer to
    // them by index. Freeing a slot would require compacting both.
    size_t occupied = 0;
    for (const auto& e : state.entries)
        if (e.state != Enrollment::Empty) ++occupied;
    w.number(occupied, 1);
    for (size_t i = 0; i < occupied; ++i) {
        const auto& e = state.entries[i];
        w.number(uint8_t(e.state), 1);
        w.number(e.node, 8);
        w.number(e.generation, 8);
        w.block(e.key.data(), e.key.size());
        w.number(e.counter, 8);
        w.number(e.receipt.counter, 8);
        w.number(e.receipt.last.size, 2);
        w.block(e.receipt.last.bytes.data(), MaxFrame);
    }
    for (size_t i = 0; i < state.count; ++i) {
        const auto& q = state.queue[i];
        w.number(q.entry, 1);
        w.number(q.frame.size, 2);
        w.block(q.frame.bytes.data(), MaxFrame);
    }
    const size_t payload = size_t(w.p - output);
    w.number(crc32(output, payload), ChecksumSize);
    return payload + ChecksumSize;
}
Health decode(const uint8_t* input, size_t size, Role role, uint64_t device, State& state) {
    if (size < MinSnapshotSize || size > SnapshotSize) return Health::Corrupt;
    Reader trailer{input + size - ChecksumSize};
    if (trailer.number(ChecksumSize) != crc32(input, size - ChecksumSize)) return Health::Corrupt;
    Reader r{input};
    uint8_t magic[sizeof(SnapshotMagic)]{};
    r.block(magic, sizeof(magic));
    if (std::memcmp(magic, SnapshotMagic, sizeof(magic)) != 0 || r.number(1) != SnapshotVersion)
        return Health::Format;
    if (r.number(1) != uint8_t(role)) return Health::Role;
    if (r.number(8) != device) return Health::Device;
    state.revision = r.number(8);
    state.network = r.number(8);
    state.receiver = r.number(8);
    state.profile = uint16_t(r.number(2));
    state.count = uint16_t(r.number(1));
    const size_t entryCount = size_t(r.number(1));
    if (!entryCount || entryCount > BindingCapacity ||
        size != MinSnapshotSize + entryCount * EntryRecordSize + state.count * QueueRecordSize)
        return Health::Invalid;
    if (!state.revision || !state.network || !state.receiver || state.profile != 1 ||
        state.count > QueueCapacity || (role == Role::Transmitter && state.count) ||
        (role == Role::Receiver && state.receiver != device))
        return Health::Invalid;
    for (size_t i = 0; i < entryCount; ++i) {
        auto& e = state.entries[i];
        e.state = Enrollment(r.number(1));
        e.node = r.number(8);
        e.generation = r.number(8);
        r.block(e.key.data(), e.key.size());
        e.counter = r.number(8);
        e.receipt.counter = r.number(8);
        e.receipt.last.size = size_t(r.number(2));
        r.block(e.receipt.last.bytes.data(), MaxFrame);
        if (uint8_t(e.state) > uint8_t(Enrollment::Revoked) || e.receipt.last.size > MaxFrame)
            return Health::Invalid;
        if (e.state == Enrollment::Empty) return Health::Invalid;
        if (!e.node || !e.generation || !nonzero(e.key) || e.node == state.receiver ||
            (role == Role::Transmitter && e.node != device) ||
            (role == Role::Transmitter && e.receipt.counter) ||
            (role == Role::Receiver && e.counter))
            return Health::Invalid;
        for (size_t j = 0; j < i; ++j) {
            const auto& prior = state.entries[j];
            if (prior.state != Enrollment::Empty &&
                (prior.key == e.key || prior.generation == e.generation ||
                 (prior.node == e.node && prior.state == Enrollment::Active &&
                  e.state == Enrollment::Active)))
                return Health::Invalid;
        }
        if (e.state == Enrollment::Prepared && (e.counter || e.receipt.counter))
            return Health::Invalid;
        if (e.receipt.counter) {
            Message message{};
            if (open(binding(e, state.network), e.receipt.last, message) != Result::Ok ||
                message.type != Type::Data || message.counter != e.receipt.counter)
                return Health::Invalid;
        } else if (e.receipt.last.size)
            return Health::Invalid;
    }
    for (size_t i = 0; i < state.count; ++i) {
        auto& q = state.queue[i];
        q.entry = uint8_t(r.number(1));
        q.frame.size = size_t(r.number(2));
        r.block(q.frame.bytes.data(), MaxFrame);
        if (q.entry >= BindingCapacity || q.frame.size > MaxFrame) return Health::Invalid;
        const auto& e = state.entries[q.entry];
        Message message{};
        if (e.state == Enrollment::Empty || e.state == Enrollment::Prepared ||
            open(binding(e, state.network), q.frame, message) != Result::Ok ||
            message.type != Type::Data || message.counter > e.receipt.counter)
            return Health::Invalid;
    }
    return Health::Ready;
}
} // namespace snapshot
} // namespace cajui
