// SPDX-License-Identifier: Apache-2.0
#include "records.h"
#include "cajui_crc32.h"
#include <cstring>

namespace cajui {
namespace records {
namespace {
enum Kind : uint8_t { RegistryKind = 'G', RetiredKind = 'X', ReceiptKind = 'R', QueueKind = 'Q' };
constexpr uint8_t HeadKind = 'H';
constexpr uint8_t Version = 1;
constexpr char Salt[] = "cajui-retired-v1";
constexpr size_t FingerprintSize = 8;
constexpr char Hex[] = "0123456789abcdef";

struct Writer {
    uint8_t* start;
    uint8_t* p;
    explicit Writer(uint8_t* output) : start(output), p(output) {}
    void number(uint64_t n, size_t size) {
        for (size_t i = size; i > 0; --i) *p++ = uint8_t(n >> ((i - 1) * 8));
    }
    void block(const uint8_t* source, size_t size) {
        std::memcpy(p, source, size);
        p += size;
    }
    size_t finish() {
        const size_t payload = size_t(p - start);
        number(crc32(start, payload), CrcSize);
        return payload + CrcSize;
    }
};
// Bounds-checked reader: every read past the end marks the record invalid.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    Reader(const uint8_t* begin, const uint8_t* finish) : p(begin), end(finish) {}
    uint64_t number(size_t size) {
        if (size_t(end - p) < size) {
            ok = false;
            p = end;
            return 0;
        }
        uint64_t n = 0;
        while (size--) n = (n << 8) | *p++;
        return n;
    }
    void block(uint8_t* destination, size_t size) {
        if (size_t(end - p) < size) {
            ok = false;
            p = end;
            return;
        }
        std::memcpy(destination, p, size);
        p += size;
    }
    bool done() const { return ok && p == end; }
};
// Checks size bounds, the trailing CRC, kind and version; returns a reader over the body.
bool open(const uint8_t* input, size_t size, size_t minimum, size_t maximum, uint8_t kind,
          Reader& reader) {
    if (!input || size < minimum || size > maximum) return false;
    Reader trailer{input + size - CrcSize, input + size};
    if (trailer.number(CrcSize) != crc32(input, size - CrcSize)) return false;
    reader = Reader(input, input + size - CrcSize);
    return reader.number(1) == kind && reader.number(1) == Version;
}
void frame(Writer& w, const Frame& f) {
    w.number(f.size, 2);
    w.block(f.bytes.data(), f.size);
}
bool frame(Reader& r, Frame& f) {
    f = Frame{};
    const size_t size = size_t(r.number(2));
    if (size > MaxFrame) return false;
    r.block(f.bytes.data(), size);
    f.size = size;
    return r.ok;
}
void name(char prefix, uint64_t index, char (&output)[NameCapacity]) {
    constexpr uint64_t Nibble = 0xf;
    output[0] = prefix;
    output[1] = Hex[(index >> 4) & Nibble];
    output[2] = Hex[index & Nibble];
    output[3] = 0;
}
// Smallest valid receipt and queue records: every field present, empty frame.
constexpr size_t MinReceipt = 1 + 1 + 8 + 8 + 8 + 2 + CrcSize;
constexpr size_t MinQueueRecord = 1 + 1 + 8 + 1 + 8 + 2 + CrcSize;
} // namespace

void receiptKey(size_t slot, char (&output)[NameCapacity]) {
    name('r', slot, output);
}
void queueKey(uint64_t sequence, char (&output)[NameCapacity]) {
    name('q', sequence % QueueCapacity, output);
}

size_t encode(const Registry& registry, Role role, uint64_t device, uint8_t* output) {
    Writer w(output);
    w.number(RegistryKind, 1);
    w.number(Version, 1);
    w.number(uint8_t(role), 1);
    w.number(device, 8);
    w.number(registry.revision, 8);
    w.number(registry.network, 8);
    w.number(registry.receiver, 8);
    w.number(registry.profile, 2);
    size_t occupied = 0;
    for (const auto& e : registry.entries)
        if (e.state != Enrollment::Empty) ++occupied;
    w.number(occupied, 1);
    for (size_t slot = 0; slot < BindingCapacity; ++slot) {
        const auto& e = registry.entries[slot];
        if (e.state == Enrollment::Empty) continue;
        w.number(slot, 1);
        w.number(uint8_t(e.state), 1);
        w.number(e.node, 8);
        w.number(e.generation, 8);
        w.block(e.key.data(), e.key.size());
        w.number(e.counter, 8);
    }
    return w.finish();
}
Health decode(const uint8_t* input, size_t size, Role role, uint64_t device, Registry& registry) {
    registry = Registry{};
    Reader r{nullptr, nullptr};
    if (!input || size < RegistryHeaderSize + CrcSize || size > RegistryCapacity)
        return Health::Corrupt;
    Reader trailer{input + size - CrcSize, input + size};
    if (trailer.number(CrcSize) != crc32(input, size - CrcSize)) return Health::Corrupt;
    r = Reader(input, input + size - CrcSize);
    if (r.number(1) != RegistryKind || r.number(1) != Version) return Health::Format;
    if (r.number(1) != uint8_t(role)) return Health::Role;
    if (r.number(8) != device) return Health::Device;
    registry.revision = r.number(8);
    registry.network = r.number(8);
    registry.receiver = r.number(8);
    registry.profile = uint16_t(r.number(2));
    const size_t count = size_t(r.number(1));
    if (count > BindingCapacity || size != RegistryHeaderSize + count * RegistryEntrySize + CrcSize)
        return Health::Invalid;
    // An empty registry is a device that left its network (factory reset).
    const bool joined = registry.network || registry.receiver || registry.profile;
    if (!registry.revision || (count && !joined) ||
        (joined && (!registry.network || !registry.receiver || registry.profile != 1)) ||
        (role == Role::Receiver && joined && registry.receiver != device))
        return Health::Invalid;
    int previous = -1;
    for (size_t i = 0; i < count; ++i) {
        const int slot = int(r.number(1));
        // Strictly increasing slots: no duplicates and a canonical encoding.
        if (slot <= previous || slot >= int(BindingCapacity)) return Health::Invalid;
        previous = slot;
        auto& e = registry.entries[size_t(slot)];
        const uint64_t state = r.number(1);
        e.node = r.number(8);
        e.generation = r.number(8);
        r.block(e.key.data(), e.key.size());
        e.counter = r.number(8);
        if (!state || state > uint8_t(Enrollment::Revoked)) return Health::Invalid;
        e.state = Enrollment(state);
        if (!e.node || !e.generation || !snapshot::nonzero(e.key) || e.node == registry.receiver ||
            (role == Role::Transmitter && e.node != device) ||
            (role == Role::Receiver && e.counter) || (e.state == Enrollment::Prepared && e.counter))
            return Health::Invalid;
    }
    return r.done() ? Health::Ready : Health::Invalid;
}
Health validate(const Registry& registry, Role role, uint64_t, const RetiredList& retired) {
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = registry.entries[i];
        if (e.state == Enrollment::Empty) continue;
        size_t active = e.state == Enrollment::Active ? 1 : 0;
        for (size_t j = 0; j < BindingCapacity; ++j) {
            const auto& other = registry.entries[j];
            if (j == i || other.state == Enrollment::Empty) continue;
            if (other.key == e.key || other.generation == e.generation) return Health::Invalid;
            if (other.node == e.node && other.state == Enrollment::Prepared &&
                e.state == Enrollment::Prepared)
                return Health::Invalid;
            if (e.state == Enrollment::Active && other.node == e.node &&
                other.state == Enrollment::Active)
                ++active;
        }
        // A receiver keeps the previous generation of a re-paired node until the node
        // uses the new one (docs/radio-pairing.md); a transmitter has one active key.
        if (active > (role == Role::Receiver ? 2u : 1u)) return Health::Invalid;
        // A revoked entry may already be retired: its slot is freed after the retired
        // list is written, and power can fail in between.
        if (e.state != Enrollment::Revoked) {
            uint64_t print = 0;
            if (!fingerprint(e.key, print)) return Health::Invalid;
            if (isRetired(retired, e.generation, print)) return Health::Invalid;
        }
    }
    return Health::Ready;
}
size_t encode(const RetiredList& list, uint8_t* output) {
    Writer w(output);
    w.number(RetiredKind, 1);
    w.number(Version, 1);
    w.number(list.count, 1);
    for (size_t i = 0; i < list.count; ++i) {
        w.number(list.items[i].generation, 8);
        w.number(list.items[i].fingerprint, 8);
    }
    return w.finish();
}
bool decode(const uint8_t* input, size_t size, RetiredList& list) {
    list = RetiredList{};
    Reader r{nullptr, nullptr};
    if (!open(input, size, 3 + CrcSize, RetiredRecordCapacity, RetiredKind, r)) return false;
    list.count = size_t(r.number(1));
    if (list.count > RetiredCapacity) {
        list = RetiredList{};
        return false;
    }
    for (size_t i = 0; i < list.count; ++i) {
        list.items[i].generation = r.number(8);
        list.items[i].fingerprint = r.number(8);
        if (!list.items[i].generation) r.ok = false;
    }
    if (r.done()) return true;
    list = RetiredList{};
    return false;
}
size_t encode(const ReceiptRecord& record, uint8_t* output) {
    Writer w(output);
    w.number(ReceiptKind, 1);
    w.number(Version, 1);
    w.number(record.generation, 8);
    w.number(record.receipt.counter, 8);
    w.number(record.through, 8);
    frame(w, record.receipt.last);
    return w.finish();
}
bool decode(const uint8_t* input, size_t size, ReceiptRecord& record) {
    record = ReceiptRecord{};
    Reader r{nullptr, nullptr};
    if (!open(input, size, MinReceipt, ReceiptCapacity, ReceiptKind, r)) return false;
    record.generation = r.number(8);
    record.receipt.counter = r.number(8);
    record.through = r.number(8);
    if (frame(r, record.receipt.last) && r.done() && record.generation) return true;
    record = ReceiptRecord{};
    return false;
}
size_t encode(const QueueRecord& record, uint8_t* output) {
    Writer w(output);
    w.number(QueueKind, 1);
    w.number(Version, 1);
    w.number(record.sequence, 8);
    w.number(record.slot, 1);
    w.number(record.generation, 8);
    frame(w, record.frame);
    return w.finish();
}
bool decode(const uint8_t* input, size_t size, QueueRecord& record) {
    record = QueueRecord{};
    Reader r{nullptr, nullptr};
    if (!open(input, size, MinQueueRecord, QueueRecordCapacity, QueueKind, r)) return false;
    record.sequence = r.number(8);
    record.slot = uint8_t(r.number(1));
    record.generation = r.number(8);
    if (frame(r, record.frame) && r.done() && record.generation && record.slot < BindingCapacity)
        return true;
    record = QueueRecord{};
    return false;
}
size_t encodeHead(uint64_t sequence, uint8_t* output) {
    Writer w(output);
    w.number(HeadKind, 1);
    w.number(Version, 1);
    w.number(sequence, 8);
    return w.finish();
}
bool decodeHead(const uint8_t* input, size_t size, uint64_t& sequence) {
    sequence = 0;
    Reader r{nullptr, nullptr};
    if (!open(input, size, HeadSize, HeadSize, HeadKind, r)) return false;
    sequence = r.number(8);
    return r.done();
}
bool fingerprint(const Key& key, uint64_t& output) {
    output = 0;
    uint8_t digest[FingerprintSize]{};
    if (!hkdfSha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(Salt),
                    sizeof(Salt) - 1, nullptr, 0, digest, sizeof(digest)))
        return false;
    for (auto byte : digest) output = (output << 8) | byte;
    return true;
}
bool isRetired(const RetiredList& list, uint64_t generation, uint64_t print) {
    for (size_t i = 0; i < list.count; ++i)
        if (list.items[i].generation == generation || list.items[i].fingerprint == print)
            return true;
    return false;
}
void retire(RetiredList& list, uint64_t generation, uint64_t print) {
    if (list.count == RetiredCapacity) {
        for (size_t i = 1; i < list.count; ++i) list.items[i - 1] = list.items[i];
        --list.count;
    }
    list.items[list.count].generation = generation;
    list.items[list.count].fingerprint = print;
    ++list.count;
}
} // namespace records
} // namespace cajui
