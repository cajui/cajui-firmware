#pragma once
#include "snapshot.h"

namespace cajui {
enum class ReadResult { Ok, Missing, Error };
class AtomicBlob {
public:
    virtual ~AtomicBlob() = default;
    virtual ReadResult read(uint8_t* output, size_t capacity, size_t& size) = 0;
    // One complete old or new blob must survive a power interruption. Never a mix.
    // A false result is ambiguous: callers must stop and reload before further use.
    virtual bool replace(const uint8_t* input, size_t size) = 0;
};
struct EnrollmentInfo {
    Enrollment state = Enrollment::Empty;
    // counter: last reserved (transmitter). received: last accepted (receiver).
    uint64_t node = 0, generation = 0, counter = 0, received = 0;
};
struct QueuedSample {
    uint64_t node = 0, generation = 0, counter = 0;
    Data data{};
};
class PersistentStore final : public CounterStore, public Journal {
public:
    PersistentStore(AtomicBlob&, Role, uint64_t device);
    // One owner per backing snapshot. Duplicating cached counters can reuse GCM nonces.
    PersistentStore(const PersistentStore&) = delete;
    PersistentStore& operator=(const PersistentStore&) = delete;
    PersistentStore(PersistentStore&&) = delete;
    PersistentStore& operator=(PersistentStore&&) = delete;
    bool mount();
    bool healthy() const { return health_ == Health::Ready; }
    Health health() const { return health_; }
    uint64_t device() const { return device_; }
    Role role() const { return role_; }
    uint64_t network() const { return state_.network; }
    uint64_t receiver() const { return state_.receiver; }
    uint16_t profile() const { return state_.profile; }
    size_t queued() const { return state_.count; }
    Result prepare(uint64_t network, uint64_t receiver, uint64_t node, uint64_t generation,
                   const Key&, uint16_t profile);
    Result activate(uint64_t node, uint64_t generation);
    Result revoke(uint64_t node, uint64_t generation);
    bool info(uint64_t node, uint64_t generation, EnrollmentInfo&) const;
    // Copies up to capacity occupied enrollments in slot order; returns how many.
    size_t list(EnrollmentInfo* output, size_t capacity) const;
    // Empty enrollment slots. Slots are never freed, so each new generation uses one.
    size_t freeSlots() const;
    bool binding(uint64_t node, Binding&) const;
    bool reserve(const Binding&, uint64_t&) override;
    bool load(const Binding&, Receipt&) override;
    Result commit(const Binding&, uint64_t expectedCounter, const Receipt&) override;
    bool peek(QueuedSample&) const;
    Result forwarded(uint64_t node, uint64_t generation, uint64_t counter);

private:
    AtomicBlob& blob_;
    Role role_;
    uint64_t device_;
    Health health_ = Health::Unmounted;
    // Scratch space is part of the object, never a large MCU task-stack allocation.
    snapshot::State state_{}, next_{};
    std::array<uint8_t, SnapshotSize> bytes_{};
    int find(uint64_t node, uint64_t generation) const;
    int authorized(const Binding&) const;
    bool save();
};
} // namespace cajui
