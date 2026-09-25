#include "cajui_storage.h"
#include <cstring>
#include <memory>
#include <new>

namespace cajui {
namespace {
Binding bindingOf(const records::Entry& e, uint64_t network) {
    Binding b{};
    b.network = network;
    b.node = e.node;
    b.key = e.key;
    b.active = true;
    return b;
}
bool frameMatches(const records::Entry& e, uint64_t network, const Frame& frame,
                  uint64_t& counter) {
    Message m{};
    if (open(bindingOf(e, network), frame, m) != Result::Ok || m.type != Type::Data) return false;
    counter = m.counter;
    return true;
}
} // namespace

PersistentStore::PersistentStore(RecordStore& records, Role role, uint64_t device)
    : records_(records), role_(role), device_(device) {}
int PersistentStore::find(uint64_t node, uint64_t generation) const {
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = registry_.entries[i];
        if (e.state != Enrollment::Empty && e.node == node && e.generation == generation)
            return int(i);
    }
    return -1;
}
int PersistentStore::authorized(const Binding& b) const {
    if (!healthy() || !b.active || b.network != registry_.network) return -1;
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = registry_.entries[i];
        if (e.state == Enrollment::Active && e.node == b.node && e.key == b.key) return int(i);
    }
    return -1;
}
void PersistentStore::clear() {
    registry_ = records::Registry{};
    retired_ = records::RetiredList{};
    for (auto& r : receipts_) r = records::ReceiptRecord{};
    queuedBySlot_.fill(0);
    head_ = tail_ = 0;
}
bool PersistentStore::fail(Health health) {
    health_ = health; // Latched until remounted: never continue after an ambiguous write.
    return false;
}
bool PersistentStore::mount() {
    health_ = Health::Unmounted;
    clear();
    if (!device_ || (role_ != Role::Transmitter && role_ != Role::Receiver))
        return fail(Health::Identity);
    const Health loaded = load();
    if (loaded != Health::Ready) {
        clear();
        return fail(loaded);
    }
    health_ = Health::Ready;
    return true;
}
Health PersistentStore::load() {
    size_t size = 0;
    auto read = records_.read(records::RegistryKey, buffer_.data(), buffer_.size(), size);
    if (read == ReadResult::Missing) {
        const Health migrated = migrate();
        if (migrated != Health::Ready) return migrated;
        read = records_.read(records::RegistryKey, buffer_.data(), buffer_.size(), size);
    }
    if (read == ReadResult::Error) return Health::ReadError;
    if (read == ReadResult::Ok) {
        const Health decoded = records::decode(buffer_.data(), size, role_, device_, registry_);
        if (decoded != Health::Ready) return decoded;
        // The registry is authoritative once written; a v1 snapshot left by an interrupted
        // clean-up is stale. Retried on every mount, so a failed erase is harmless.
        records_.erase(records::LegacyKey);
    }
    read = records_.read(records::RetiredKey, buffer_.data(), buffer_.size(), size);
    if (read == ReadResult::Error) return Health::ReadError;
    if (read == ReadResult::Ok && !records::decode(buffer_.data(), size, retired_))
        return Health::Corrupt;
    const Health valid = records::validate(registry_, role_, device_, retired_);
    if (valid != Health::Ready) return valid;
    return role_ == Role::Receiver ? loadReceiver() : Health::Ready;
}
Health PersistentStore::loadReceiver() {
    size_t size = 0;
    auto read = records_.read(records::HeadKey, buffer_.data(), buffer_.size(), size);
    if (read == ReadResult::Error) return Health::ReadError;
    if (read == ReadResult::Ok && !records::decodeHead(buffer_.data(), size, head_))
        return Health::Corrupt;
    tail_ = head_;
    for (size_t slot = 0; slot < BindingCapacity; ++slot) {
        const auto& e = registry_.entries[slot];
        if (e.state == Enrollment::Empty) continue;
        char name[records::NameCapacity];
        records::receiptKey(slot, name);
        read = records_.read(name, buffer_.data(), buffer_.size(), size);
        if (read == ReadResult::Error) return Health::ReadError;
        if (read == ReadResult::Missing) continue;
        records::ReceiptRecord record{};
        if (!records::decode(buffer_.data(), size, record)) return Health::Corrupt;
        // Left by a retired credential that used this slot before: not this entry's.
        if (record.generation != e.generation) continue;
        uint64_t counter = 0;
        if (e.state == Enrollment::Prepared || !record.receipt.counter ||
            !frameMatches(e, registry_.network, record.receipt.last, counter) ||
            counter != record.receipt.counter)
            return Health::Invalid;
        receipts_[slot] = record;
        if (record.through > tail_) tail_ = record.through;
    }
    if (tail_ - head_ > QueueCapacity) return Health::Invalid;
    // Every sequence in [head, tail) must be a valid queued sample: a missing or damaged
    // record is corruption, never an empty queue.
    for (uint64_t sequence = head_; sequence != tail_; ++sequence) {
        char name[records::NameCapacity];
        records::queueKey(sequence, name);
        read = records_.read(name, buffer_.data(), buffer_.size(), size);
        if (read == ReadResult::Error) return Health::ReadError;
        records::QueueRecord record{};
        if (read == ReadResult::Missing) return Health::Invalid;
        if (!records::decode(buffer_.data(), size, record)) return Health::Corrupt;
        const auto& e = registry_.entries[record.slot];
        uint64_t counter = 0;
        if (record.sequence != sequence || e.state == Enrollment::Empty ||
            e.state == Enrollment::Prepared || e.generation != record.generation ||
            !frameMatches(e, registry_.network, record.frame, counter) ||
            counter > receipts_[record.slot].receipt.counter)
            return Health::Invalid;
        ++queuedBySlot_[record.slot];
    }
    return Health::Ready;
}
// v1 kept everything in one snapshot. It is copied into records, the registry last: until
// the registry exists, a restart simply migrates again from the untouched snapshot.
Health PersistentStore::migrate() {
    std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[SnapshotSize]);
    std::unique_ptr<snapshot::State> state(new (std::nothrow) snapshot::State());
    if (!bytes || !state) return Health::ReadError;
    size_t size = 0;
    const auto read = records_.read(records::LegacyKey, bytes.get(), SnapshotSize, size);
    if (read == ReadResult::Missing) return Health::Ready; // A new device: nothing stored.
    if (read == ReadResult::Error) return Health::ReadError;
    snapshot::reset(*state);
    const Health decoded = snapshot::decode(bytes.get(), size, role_, device_, *state);
    if (decoded != Health::Ready) return decoded;
    next_ = records::Registry{};
    next_.revision = state->revision;
    next_.network = state->network;
    next_.receiver = state->receiver;
    next_.profile = state->profile;
    for (size_t slot = 0; slot < BindingCapacity; ++slot) {
        const auto& old = state->entries[slot];
        auto& e = next_.entries[slot];
        e.state = old.state;
        e.node = old.node;
        e.generation = old.generation;
        e.key = old.key;
        e.counter = old.counter;
    }
    if (role_ == Role::Receiver) {
        std::array<uint64_t, BindingCapacity> through{};
        for (size_t sequence = 0; sequence < state->count; ++sequence) {
            records::QueueRecord record{};
            record.sequence = sequence;
            record.slot = state->queue[sequence].entry;
            record.generation = state->entries[record.slot].generation;
            record.frame = state->queue[sequence].frame;
            through[record.slot] = sequence + 1;
            char name[records::NameCapacity];
            records::queueKey(sequence, name);
            if (!records_.write(name, buffer_.data(), records::encode(record, buffer_.data())))
                return Health::WriteError;
        }
        for (size_t slot = 0; slot < BindingCapacity; ++slot) {
            const auto& old = state->entries[slot];
            if (old.state == Enrollment::Empty || !old.receipt.counter) continue;
            records::ReceiptRecord record{};
            record.generation = old.generation;
            record.receipt = old.receipt;
            record.through = through[slot];
            char name[records::NameCapacity];
            records::receiptKey(slot, name);
            if (!records_.write(name, buffer_.data(), records::encode(record, buffer_.data())))
                return Health::WriteError;
        }
        if (!records_.write(records::HeadKey, buffer_.data(),
                            records::encodeHead(0, buffer_.data())))
            return Health::WriteError;
    }
    const size_t encoded = records::encode(next_, role_, device_, buffer_.data());
    return records_.write(records::RegistryKey, buffer_.data(), encoded) ? Health::Ready
                                                                         : Health::WriteError;
}
bool PersistentStore::saveRegistry() {
    if (registry_.revision == UINT64_MAX) return fail(Health::WriteError);
    next_.revision = registry_.revision + 1;
    const size_t size = records::encode(next_, role_, device_, buffer_.data());
    if (!records_.write(records::RegistryKey, buffer_.data(), size))
        return fail(Health::WriteError); // Never reuse counters after an ambiguous write.
    registry_ = next_;
    return true;
}
bool PersistentStore::saveRetired() {
    const size_t size = records::encode(nextRetired_, buffer_.data());
    if (!records_.write(records::RetiredKey, buffer_.data(), size)) return fail(Health::WriteError);
    retired_ = nextRetired_;
    return true;
}
bool PersistentStore::saveHead(uint64_t sequence) {
    if (!records_.write(records::HeadKey, buffer_.data(),
                        records::encodeHead(sequence, buffer_.data())))
        return fail(Health::WriteError);
    head_ = sequence;
    return true;
}
int PersistentStore::reclaimable() const {
    for (size_t i = 0; i < BindingCapacity; ++i)
        if (registry_.entries[i].state == Enrollment::Revoked && !queuedBySlot_[i]) return int(i);
    return -1;
}
Result PersistentStore::prepare(uint64_t network, uint64_t receiver, uint64_t node,
                                uint64_t generation, const Key& key, uint16_t profile) {
    if (!healthy()) return Result::StorageError;
    if (!network || !receiver || !node || !generation || !snapshot::nonzero(key) ||
        receiver == node || profile != 1 || (role_ == Role::Transmitter && node != device_) ||
        (role_ == Role::Receiver && receiver != device_))
        return Result::Invalid;
    if (registry_.network && (network != registry_.network || receiver != registry_.receiver ||
                              profile != registry_.profile))
        return Result::Conflict;
    const int existing = find(node, generation);
    if (existing >= 0) {
        const auto& e = registry_.entries[size_t(existing)];
        return e.key == key && e.state != Enrollment::Revoked ? Result::Ok : Result::Conflict;
    }
    uint64_t print = 0;
    if (!records::fingerprint(key, print)) return Result::CryptoError;
    if (records::isRetired(retired_, generation, print)) return Result::Conflict;
    int empty = -1;
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto& e = registry_.entries[i];
        if (e.state == Enrollment::Empty) {
            if (empty < 0) empty = int(i);
            continue;
        }
        if (e.key == key || e.generation == generation ||
            (e.node == node && e.state == Enrollment::Prepared))
            return Result::Conflict;
    }
    if (empty < 0) {
        // Free a revoked slot: its credential joins the retired list first, so a power cut
        // before the registry write leaves it both revoked and retired, never reusable.
        empty = reclaimable();
        if (empty < 0) return Result::Full;
        const auto& old = registry_.entries[size_t(empty)];
        uint64_t oldPrint = 0;
        if (!records::fingerprint(old.key, oldPrint)) return Result::CryptoError;
        nextRetired_ = retired_;
        records::retire(nextRetired_, old.generation, oldPrint);
        if (!saveRetired()) return Result::StorageError;
    }
    next_ = registry_;
    next_.network = network;
    next_.receiver = receiver;
    next_.profile = profile;
    auto& e = next_.entries[size_t(empty)];
    e = records::Entry{};
    e.state = Enrollment::Prepared;
    e.node = node;
    e.generation = generation;
    e.key = key;
    if (!saveRegistry()) return Result::StorageError;
    receipts_[size_t(empty)] = records::ReceiptRecord{};
    return Result::Ok;
}
Result PersistentStore::activate(uint64_t node, uint64_t generation) {
    if (!healthy()) return Result::StorageError;
    const int index = find(node, generation);
    if (index < 0) return Result::NotFound;
    const auto& old = registry_.entries[size_t(index)];
    if (old.state == Enrollment::Active) return Result::Ok;
    if (old.state != Enrollment::Prepared) return Result::Conflict;
    next_ = registry_;
    for (auto& e : next_.entries)
        if (e.node == node && e.state == Enrollment::Active) e.state = Enrollment::Revoked;
    next_.entries[size_t(index)].state = Enrollment::Active;
    return saveRegistry() ? Result::Ok : Result::StorageError;
}
Result PersistentStore::activateAlongside(uint64_t node, uint64_t generation) {
    if (!healthy()) return Result::StorageError;
    if (role_ != Role::Receiver) return Result::Invalid;
    const int index = find(node, generation);
    if (index < 0) return Result::NotFound;
    const auto& target = registry_.entries[size_t(index)];
    if (target.state == Enrollment::Active) return Result::Ok;
    if (target.state != Enrollment::Prepared) return Result::Conflict;
    next_ = registry_;
    // Keep only a previous generation the node has actually used: at most two stay active.
    for (size_t i = 0; i < BindingCapacity; ++i) {
        auto& e = next_.entries[i];
        if (e.node == node && e.state == Enrollment::Active && !receipts_[i].receipt.counter)
            e.state = Enrollment::Revoked;
    }
    next_.entries[size_t(index)].state = Enrollment::Active;
    return saveRegistry() ? Result::Ok : Result::StorageError;
}
Result PersistentStore::revoke(uint64_t node, uint64_t generation) {
    if (!healthy()) return Result::StorageError;
    const int index = find(node, generation);
    if (index < 0) return Result::NotFound;
    if (registry_.entries[size_t(index)].state == Enrollment::Revoked) return Result::Ok;
    next_ = registry_;
    next_.entries[size_t(index)].state = Enrollment::Revoked;
    return saveRegistry() ? Result::Ok : Result::StorageError;
}
bool PersistentStore::info(uint64_t node, uint64_t generation, EnrollmentInfo& out) const {
    out = EnrollmentInfo{};
    if (!healthy()) return false;
    const int index = find(node, generation);
    if (index < 0) return false;
    const auto& e = registry_.entries[size_t(index)];
    out.state = e.state;
    out.node = e.node;
    out.generation = e.generation;
    out.counter = e.counter;
    out.received = receipts_[size_t(index)].receipt.counter;
    return true;
}
size_t PersistentStore::freeSlots() const {
    if (!healthy()) return 0;
    size_t count = 0;
    for (size_t i = 0; i < BindingCapacity; ++i) {
        const auto state = registry_.entries[i].state;
        if (state == Enrollment::Empty || (state == Enrollment::Revoked && !queuedBySlot_[i]))
            ++count;
    }
    return count;
}
size_t PersistentStore::list(EnrollmentInfo* output, size_t capacity) const {
    size_t count = 0;
    if (!healthy() || !output) return 0;
    for (size_t i = 0; i < BindingCapacity && count < capacity; ++i) {
        const auto& e = registry_.entries[i];
        if (e.state == Enrollment::Empty) continue;
        auto& info = output[count++];
        info.state = e.state;
        info.node = e.node;
        info.generation = e.generation;
        info.counter = e.counter;
        info.received = receipts_[i].receipt.counter;
    }
    return count;
}
bool PersistentStore::binding(uint64_t node, Binding& out) const {
    out = Binding{};
    if (!healthy()) return false;
    for (const auto& e : registry_.entries)
        if (e.node == node && e.state == Enrollment::Active) {
            out = bindingOf(e, registry_.network);
            return true;
        }
    return false;
}
size_t PersistentStore::bindings(uint64_t node, Binding* output, size_t capacity) const {
    size_t count = 0;
    if (!healthy() || !output) return 0;
    for (const auto& e : registry_.entries)
        if (e.node == node && e.state == Enrollment::Active && count < capacity)
            output[count++] = bindingOf(e, registry_.network);
    return count;
}
bool PersistentStore::reserve(const Binding& b, uint64_t& out) {
    out = 0;
    const int index = authorized(b);
    if (index < 0 || role_ != Role::Transmitter ||
        registry_.entries[size_t(index)].counter == UINT64_MAX)
        return false;
    next_ = registry_;
    ++next_.entries[size_t(index)].counter;
    if (!saveRegistry()) return false;
    out = registry_.entries[size_t(index)].counter;
    return true;
}
bool PersistentStore::load(const Binding& b, Receipt& out) {
    out = Receipt{};
    const int index = authorized(b);
    if (index < 0 || role_ != Role::Receiver) return false;
    out = receipts_[size_t(index)].receipt;
    return true;
}
// Two writes, no rewrite of other state: the queue record first, unreferenced, then the
// receipt that references it. The receipt write is the commit point.
Result PersistentStore::commit(const Binding& b, uint64_t expected, const Receipt& receipt) {
    if (!healthy()) return Result::StorageError;
    const int index = authorized(b);
    if (index < 0 || role_ != Role::Receiver) return Result::Unauthorized;
    const size_t slot = size_t(index);
    if (receipts_[slot].receipt.counter != expected) return Result::Conflict;
    Message m{};
    if (open(b, receipt.last, m) != Result::Ok || m.type != Type::Data ||
        m.counter != receipt.counter || m.counter <= expected)
        return Result::Invalid;
    if (queued() == QueueCapacity) return Result::Full;
    records::QueueRecord record{};
    record.sequence = tail_;
    record.slot = uint8_t(slot);
    record.generation = registry_.entries[slot].generation;
    record.frame = receipt.last;
    char name[records::NameCapacity];
    records::queueKey(tail_, name);
    if (!records_.write(name, buffer_.data(), records::encode(record, buffer_.data()))) {
        fail(Health::WriteError);
        return Result::StorageError;
    }
    records::ReceiptRecord next{};
    next.generation = record.generation;
    next.receipt = receipt;
    next.through = tail_ + 1;
    records::receiptKey(slot, name);
    if (!records_.write(name, buffer_.data(), records::encode(next, buffer_.data()))) {
        fail(Health::WriteError);
        return Result::StorageError;
    }
    receipts_[slot] = next;
    ++tail_;
    ++queuedBySlot_[slot];
    // The node used a re-paired generation: its previous one is revoked now, not at
    // pairing, so a lost JOIN_DONE never cuts the node off. The sample is already durable.
    bool superseded = false;
    next_ = registry_;
    for (size_t i = 0; i < BindingCapacity; ++i) {
        auto& e = next_.entries[i];
        if (i != slot && e.node == b.node && e.state == Enrollment::Active) {
            e.state = Enrollment::Revoked;
            superseded = true;
        }
    }
    if (superseded && !saveRegistry()) return Result::StorageError;
    return Result::Ok;
}
Health PersistentStore::readFront(records::QueueRecord& record) {
    record = records::QueueRecord{};
    char name[records::NameCapacity];
    records::queueKey(head_, name);
    size_t size = 0;
    const auto read = records_.read(name, buffer_.data(), buffer_.size(), size);
    if (read != ReadResult::Ok) return Health::ReadError;
    if (!records::decode(buffer_.data(), size, record)) return Health::Corrupt;
    const auto& e = registry_.entries[record.slot];
    if (record.sequence != head_ || e.state == Enrollment::Empty ||
        e.generation != record.generation)
        return Health::Invalid;
    return Health::Ready;
}
bool PersistentStore::peek(QueuedSample& out) {
    out = QueuedSample{};
    if (!healthy() || !queued()) return false;
    records::QueueRecord record{};
    const Health front = readFront(record);
    if (front != Health::Ready) return fail(front); // It validated at mount.
    const auto& e = registry_.entries[record.slot];
    Message m{};
    if (open(bindingOf(e, registry_.network), record.frame, m) != Result::Ok)
        return fail(Health::Invalid);
    out.node = e.node;
    out.generation = e.generation;
    out.counter = m.counter;
    out.data = m.data;
    return true;
}
Result PersistentStore::forwarded(uint64_t node, uint64_t generation, uint64_t counter) {
    if (!healthy()) return Result::StorageError;
    QueuedSample front{};
    if (!peek(front)) return healthy() ? Result::Conflict : Result::StorageError;
    if (front.node != node || front.generation != generation || front.counter != counter)
        return Result::Conflict;
    const int slot = find(node, generation);
    if (!saveHead(head_ + 1)) return Result::StorageError;
    --queuedBySlot_[size_t(slot)];
    return Result::Ok;
}
Result PersistentStore::reset(bool discardQueue) {
    if (!healthy()) return Result::StorageError;
    if (queued() && !discardQueue) return Result::Conflict;
    // Revoke, retire, drop the queue, then empty the registry. Every intermediate state is
    // valid and none leaves an old credential usable or forgotten; repeating the reset
    // after a power cut completes it. Queue sequences continue from the tail.
    next_ = registry_;
    nextRetired_ = retired_;
    for (auto& e : next_.entries) {
        if (e.state == Enrollment::Empty) continue;
        e.state = Enrollment::Revoked;
        uint64_t print = 0;
        if (!records::fingerprint(e.key, print)) return Result::CryptoError;
        if (!records::isRetired(nextRetired_, e.generation, print))
            records::retire(nextRetired_, e.generation, print);
    }
    if (!saveRegistry() || !saveRetired() || (queued() && !saveHead(tail_)))
        return Result::StorageError;
    next_ = records::Registry{};
    if (!saveRegistry()) return Result::StorageError;
    for (auto& r : receipts_) r = records::ReceiptRecord{};
    queuedBySlot_.fill(0);
    return Result::Ok;
}
} // namespace cajui
