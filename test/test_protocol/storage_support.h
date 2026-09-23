#pragma once
#include "cajui_storage.h"
#include <cstring>
#include <memory>
#include <vector>
namespace fixtures {
using namespace cajui;
class MemoryBlob : public AtomicBlob {
public:
    std::vector<uint8_t> bytes = std::vector<uint8_t>(SnapshotSize);
    size_t size = 0, writes = 0;
    bool failRead = false, failBefore = false, failAfter = false, tear = false;
    ReadResult read(uint8_t* out, size_t cap, size_t& n) override {
        n = size;
        if (failRead || size > cap) return ReadResult::Error;
        if (!size) return ReadResult::Missing;
        std::memcpy(out, bytes.data(), size);
        return ReadResult::Ok;
    }
    bool replace(const uint8_t* in, size_t n) override {
        if (failBefore) return false;
        size = n;
        std::memcpy(bytes.data(), in, n);
        ++writes;
        if (tear) {
            bytes[20] ^= 1;
            return false;
        }
        return !failAfter;
    }
};
inline void repairChecksum(MemoryBlob& blob) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < blob.size - 4; ++i) {
        crc ^= blob.bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    crc = ~crc;
    for (size_t i = 0; i < 4; ++i) blob.bytes[blob.size - 4 + i] = uint8_t(crc >> ((3 - i) * 8));
}
inline Key key(uint8_t value = 1) {
    Key k{};
    k.fill(value);
    return k;
}
inline std::unique_ptr<PersistentStore> mounted(MemoryBlob& blob, Role role = Role::Transmitter) {
    std::unique_ptr<PersistentStore> store(
        new PersistentStore(blob, role, role == Role::Transmitter ? 2 : 1));
    store->mount();
    return store;
}
inline bool enroll(PersistentStore& store, uint64_t node = 2, uint64_t gen = 10,
                   uint8_t secret = 1) {
    return store.prepare(42, 1, node, gen, key(secret), 1) == Result::Ok &&
           store.activate(node, gen) == Result::Ok;
}
inline Data sample() {
    Data d{};
    d.batteryMv = 3900;
    d.nextSeconds = 300;
    d.count = 1;
    d.readings[0].sensor = 1;
    d.readings[0].metric = 1;
    d.readings[0].unit = 1;
    d.readings[0].milliValue = 25000;
    return d;
}
inline Binding binding(uint64_t node = 2, uint8_t secret = 1) {
    Binding b{};
    b.network = 42;
    b.node = node;
    b.key = key(secret);
    b.active = true;
    return b;
}
inline Frame data(uint64_t counter, uint64_t node = 2, uint8_t secret = 1) {
    Frame f{};
    Message m{};
    m.counter = counter;
    m.data = sample();
    seal(binding(node, secret), m, f);
    return f;
}
}
