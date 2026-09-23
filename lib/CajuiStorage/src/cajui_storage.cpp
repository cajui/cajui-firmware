#include "cajui_storage.h"
#include <cstring>

namespace cajui {
namespace {
// CRC detects accidental snapshot corruption; it is not authentication.
uint32_t checksum(const uint8_t* data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
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
bool nonzero(const Key& key) {
    for (auto byte : key)
        if (byte) return true;
    return false;
}
}
PersistentStore::PersistentStore(AtomicBlob& blob, Role role, uint64_t device)
    : blob_(blob), role_(role), device_(device) {}
Binding PersistentStore::asBinding(const Entry& e, uint64_t network) const {
    Binding b{};
    b.network = network;
    b.node = e.node;
    b.key = e.key;
    b.active = true;
    return b;
}
int PersistentStore::find(uint64_t node, uint64_t generation) const {
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = state_.entries[i];
        if (e.state != Enrollment::Empty && e.node == node && e.generation == generation)
            return int(i);
    }
    return -1;
}
int PersistentStore::authorized(const Binding& b) const {
    if (!healthy() || !b.active || b.network != state_.network) return -1;
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = state_.entries[i];
        if (e.state == Enrollment::Active && e.node == b.node && e.key == b.key) return int(i);
    }
    return -1;
}
bool PersistentStore::mount() {
    health_ = Health::Unmounted;
    if (!device_ || (role_ != Role::Transmitter && role_ != Role::Receiver)) {
        health_ = Health::Identity;
        return false;
    }
    size_t size = 0;
    const auto result = blob_.read(bytes_.data(), bytes_.size(), size);
    bytesSize_ = size;
    // Reset in place: `next_ = State{}` may build a ~22 KB temporary on the 8 KB loop-task stack.
    next_.revision = next_.network = next_.receiver = 0;
    next_.profile = next_.count = 0;
    for (auto& e : next_.entries) e = Entry{};
    for (auto& q : next_.queue) q = Queued{};
    const Health loaded = result == ReadResult::Error                     ? Health::ReadError
                          : result == ReadResult::Missing                 ? Health::Ready
                          : size < MinSnapshotSize || size > SnapshotSize ? Health::Corrupt
                                                                          : decode();
    if (loaded != Health::Ready) {
        health_ = loaded;
        return false;
    }
    state_ = next_;
    health_ = Health::Ready;
    return true;
}
void PersistentStore::encode() {
    bytes_.fill(0);
    Writer w{bytes_.data()};
    w.block(SnapshotMagic, sizeof(SnapshotMagic));
    w.number(SnapshotVersion, 1);
    w.number(uint8_t(role_), 1);
    w.number(device_, 8);
    w.number(next_.revision, 8);
    w.number(next_.network, 8);
    w.number(next_.receiver, 8);
    w.number(next_.profile, 2);
    w.number(next_.count, 1); // count <= 128
    // Slots are never freed, so occupied entries form a prefix; queued frames refer to
    // them by index. Freeing a slot would require compacting both.
    size_t occupied = 0;
    for (const auto& e : next_.entries)
        if (e.state != Enrollment::Empty) ++occupied;
    w.number(occupied, 1);
    for (size_t i = 0; i < occupied; ++i) {
        const auto& e = next_.entries[i];
        w.number(uint8_t(e.state), 1);
        w.number(e.node, 8);
        w.number(e.generation, 8);
        w.block(e.key.data(), e.key.size());
        w.number(e.counter, 8);
        w.number(e.receipt.counter, 8);
        w.number(e.receipt.last.size, 2);
        w.block(e.receipt.last.bytes.data(), MaxFrame);
    }
    for (size_t i = 0; i < next_.count; ++i) {
        const auto& q = next_.queue[i];
        w.number(q.entry, 1);
        w.number(q.frame.size, 2);
        w.block(q.frame.bytes.data(), MaxFrame);
    }
    bytesSize_ = size_t(w.p - bytes_.data()) + 4;
    w.number(checksum(bytes_.data(), bytesSize_ - 4), 4);
}
Health PersistentStore::decode() {
    Reader trailer{bytes_.data() + bytesSize_ - 4};
    if (trailer.number(4) != checksum(bytes_.data(), bytesSize_ - 4)) return Health::Corrupt;
    Reader r{bytes_.data()};
    uint8_t magic[sizeof(SnapshotMagic)]{};
    r.block(magic, sizeof(magic));
    if (std::memcmp(magic, SnapshotMagic, sizeof(magic)) || r.number(1) != SnapshotVersion)
        return Health::Format;
    if (r.number(1) != uint8_t(role_)) return Health::Role;
    if (r.number(8) != device_) return Health::Device;
    next_.revision = r.number(8);
    next_.network = r.number(8);
    next_.receiver = r.number(8);
    next_.profile = uint16_t(r.number(2));
    next_.count = uint16_t(r.number(1));
    const size_t entryCount = size_t(r.number(1));
    if (!entryCount || entryCount > BindingCapacity ||
        bytesSize_ !=
            MinSnapshotSize + entryCount * EntryRecordSize + next_.count * QueueRecordSize)
        return Health::Invalid;
    if (!next_.revision || !next_.network || !next_.receiver || next_.profile != 1 ||
        next_.count > QueueCapacity || (role_ == Role::Transmitter && next_.count) ||
        (role_ == Role::Receiver && next_.receiver != device_))
        return Health::Invalid;
    for (size_t i = 0; i < entryCount; ++i) {
        auto& e = next_.entries[i];
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
        if (!e.node || !e.generation || !nonzero(e.key) || e.node == next_.receiver ||
            (role_ == Role::Transmitter && e.node != device_) ||
            (role_ == Role::Transmitter && e.receipt.counter) ||
            (role_ == Role::Receiver && e.counter))
            return Health::Invalid;
        for (size_t j = 0; j < i; ++j) {
            const auto& prior = next_.entries[j];
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
            if (open(asBinding(e, next_.network), e.receipt.last, message) != Result::Ok ||
                message.type != Type::Data || message.counter != e.receipt.counter)
                return Health::Invalid;
        } else if (e.receipt.last.size)
            return Health::Invalid;
    }
    for (size_t i = 0; i < next_.count; ++i) {
        auto& q = next_.queue[i];
        q.entry = uint8_t(r.number(1));
        q.frame.size = size_t(r.number(2));
        r.block(q.frame.bytes.data(), MaxFrame);
        if (q.entry >= BindingCapacity || q.frame.size > MaxFrame) return Health::Invalid;
        const auto& e = next_.entries[q.entry];
        Message message{};
        if (e.state == Enrollment::Empty || e.state == Enrollment::Prepared ||
            open(asBinding(e, next_.network), q.frame, message) != Result::Ok ||
            message.type != Type::Data || message.counter > e.receipt.counter)
            return Health::Invalid;
    }
    return Health::Ready;
}
bool PersistentStore::save() {
    if (state_.revision == UINT64_MAX) {
        health_ = Health::WriteError;
        return false;
    }
    next_.revision = state_.revision + 1;
    encode();
    if (!blob_.replace(bytes_.data(), bytesSize_)) {
        health_ =
            Health::WriteError; // An ambiguous write must never lead to reuse of old counters.
        return false;
    }
    state_ = next_;
    return true;
}
Result PersistentStore::prepare(uint64_t network, uint64_t receiver, uint64_t node,
                                uint64_t generation, const Key& key, uint16_t profile) {
    if (!healthy()) return Result::StorageError;
    if (!network || !receiver || !node || !generation || !nonzero(key) || receiver == node ||
        profile != 1 || (role_ == Role::Transmitter && node != device_) ||
        (role_ == Role::Receiver && receiver != device_))
        return Result::Invalid;
    if (state_.network &&
        (network != state_.network || receiver != state_.receiver || profile != state_.profile))
        return Result::Conflict;
    const int existing = find(node, generation);
    if (existing >= 0) {
        const auto& e = state_.entries[size_t(existing)];
        return e.key == key && e.state != Enrollment::Revoked ? Result::Ok : Result::Conflict;
    }
    int empty = -1;
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = state_.entries[i];
        if (e.state == Enrollment::Empty) {
            if (empty < 0) empty = int(i);
            continue;
        }
        if (e.key == key || e.generation == generation ||
            (e.node == node && e.state == Enrollment::Prepared))
            return Result::Conflict;
    }
    if (empty < 0) return Result::Full;
    next_ = state_;
    next_.network = network;
    next_.receiver = receiver;
    next_.profile = profile;
    auto& e = next_.entries[size_t(empty)];
    e.state = Enrollment::Prepared;
    e.node = node;
    e.generation = generation;
    e.key = key;
    return save() ? Result::Ok : Result::StorageError;
}
Result PersistentStore::activate(uint64_t node, uint64_t generation) {
    if (!healthy()) return Result::StorageError;
    const int index = find(node, generation);
    if (index < 0) return Result::NotFound;
    const auto& old = state_.entries[size_t(index)];
    if (old.state == Enrollment::Active) return Result::Ok;
    if (old.state != Enrollment::Prepared) return Result::Conflict;
    next_ = state_;
    for (auto& e : next_.entries)
        if (e.node == node && e.state == Enrollment::Active) e.state = Enrollment::Revoked;
    next_.entries[size_t(index)].state = Enrollment::Active;
    return save() ? Result::Ok : Result::StorageError;
}
Result PersistentStore::revoke(uint64_t node, uint64_t generation) {
    if (!healthy()) return Result::StorageError;
    const int index = find(node, generation);
    if (index < 0) return Result::NotFound;
    if (state_.entries[size_t(index)].state == Enrollment::Revoked) return Result::Ok;
    next_ = state_;
    next_.entries[size_t(index)].state = Enrollment::Revoked;
    return save() ? Result::Ok : Result::StorageError;
}
bool PersistentStore::info(uint64_t node, uint64_t generation, EnrollmentInfo& out) const {
    out = EnrollmentInfo{};
    if (!healthy()) return false;
    const int index = find(node, generation);
    if (index < 0) return false;
    const auto& e = state_.entries[size_t(index)];
    out.state = e.state;
    out.node = e.node;
    out.generation = e.generation;
    out.counter = e.counter;
    return true;
}
bool PersistentStore::binding(uint64_t node, Binding& out) const {
    out = Binding{};
    if (!healthy()) return false;
    for (const auto& e : state_.entries)
        if (e.node == node && e.state == Enrollment::Active) {
            out = asBinding(e, state_.network);
            return true;
        }
    return false;
}
bool PersistentStore::reserve(const Binding& b, uint64_t& out) {
    out = 0;
    const int index = authorized(b);
    if (index < 0 || role_ != Role::Transmitter ||
        state_.entries[size_t(index)].counter == UINT64_MAX)
        return false;
    next_ = state_;
    ++next_.entries[size_t(index)].counter;
    if (!save()) return false;
    out = state_.entries[size_t(index)].counter;
    return true;
}
bool PersistentStore::load(const Binding& b, Receipt& out) {
    out = Receipt{};
    const int index = authorized(b);
    if (index < 0 || role_ != Role::Receiver) return false;
    out = state_.entries[size_t(index)].receipt;
    return true;
}
Result PersistentStore::commit(const Binding& b, uint64_t expected, const Receipt& receipt,
                               const Data&) {
    const int index = authorized(b);
    if (index < 0 || role_ != Role::Receiver) return Result::Unauthorized;
    if (state_.entries[size_t(index)].receipt.counter != expected) return Result::Conflict;
    Message m{};
    if (open(b, receipt.last, m) != Result::Ok || m.type != Type::Data ||
        m.counter != receipt.counter || m.counter <= expected)
        return Result::Invalid;
    if (state_.count == QueueCapacity) return Result::Full;
    next_ = state_;
    next_.entries[size_t(index)].receipt = receipt;
    auto& q = next_.queue[next_.count++];
    q.entry = uint8_t(index);
    q.frame = receipt.last;
    return save() ? Result::Ok : Result::StorageError;
}
bool PersistentStore::peek(QueuedSample& out) const {
    out = QueuedSample{};
    if (!healthy() || !state_.count) return false;
    const auto& q = state_.queue[0];
    const auto& e = state_.entries[q.entry];
    Message m{};
    if (open(asBinding(e, state_.network), q.frame, m) != Result::Ok) return false;
    out.node = e.node;
    out.generation = e.generation;
    out.counter = m.counter;
    out.data = m.data;
    return true;
}
Result PersistentStore::forwarded(uint64_t node, uint64_t generation, uint64_t counter) {
    if (!healthy()) return Result::StorageError;
    QueuedSample front{};
    if (!peek(front) || front.node != node || front.generation != generation ||
        front.counter != counter)
        return Result::Conflict;
    next_ = state_;
    for (size_t i = 1; i < next_.count; ++i) next_.queue[i - 1] = next_.queue[i];
    next_.queue[--next_.count] = Queued{};
    return save() ? Result::Ok : Result::StorageError;
}
} // namespace cajui
