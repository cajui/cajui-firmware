#include "cajui_storage.h"
#include <cstring>

namespace cajui {
PersistentStore::PersistentStore(AtomicBlob& blob, Role role, uint64_t device)
    : blob_(blob), role_(role), device_(device) {}
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
    snapshot::reset(next_);
    const Health loaded = result == ReadResult::Error ? Health::ReadError
                          : result == ReadResult::Missing
                              ? Health::Ready
                              : snapshot::decode(bytes_.data(), size, role_, device_, next_);
    if (loaded != Health::Ready) {
        health_ = loaded;
        return false;
    }
    state_ = next_;
    health_ = Health::Ready;
    return true;
}
bool PersistentStore::save() {
    if (state_.revision == UINT64_MAX) {
        health_ = Health::WriteError;
        return false;
    }
    next_.revision = state_.revision + 1;
    const size_t size = snapshot::encode(next_, role_, device_, bytes_.data());
    if (!blob_.replace(bytes_.data(), size)) {
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
    if (!network || !receiver || !node || !generation || !snapshot::nonzero(key) ||
        receiver == node || profile != 1 || (role_ == Role::Transmitter && node != device_) ||
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
            out = snapshot::binding(e, state_.network);
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
Result PersistentStore::commit(const Binding& b, uint64_t expected, const Receipt& receipt) {
    if (!healthy()) return Result::StorageError;
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
    if (open(snapshot::binding(e, state_.network), q.frame, m) != Result::Ok) return false;
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
    next_.queue[--next_.count] = snapshot::Queued{};
    return save() ? Result::Ok : Result::StorageError;
}
} // namespace cajui
