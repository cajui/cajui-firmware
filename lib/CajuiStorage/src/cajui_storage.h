#pragma once
#include "records.h"

namespace cajui {
enum class ReadResult { Ok, Missing, Error };
// One blob, replaced as a whole (the receiver uplink settings).
class AtomicBlob {
public:
    virtual ~AtomicBlob() = default;
    virtual ReadResult read(uint8_t* output, size_t capacity, size_t& size) = 0;
    // One complete old or new blob must survive a power interruption. Never a mix.
    // A false result is ambiguous: callers must stop and reload before further use.
    virtual bool replace(const uint8_t* input, size_t size) = 0;
};
// Named records, each replaced atomically and independently (NVS keys on the device).
class RecordStore {
public:
    virtual ~RecordStore() = default;
    virtual ReadResult read(const char* name, uint8_t* output, size_t capacity, size_t& size) = 0;
    // One complete old or new record must survive a power interruption. Never a mix.
    // A false result is ambiguous: the write may or may not have become durable.
    virtual bool write(const char* name, const uint8_t* input, size_t size) = 0;
    // Removing a missing record succeeds. A false result is ambiguous.
    virtual bool erase(const char* name) = 0;
};
// A single record of a RecordStore used as a blob; sizes outside the bounds are refused.
class RecordBlob final : public AtomicBlob {
public:
    RecordBlob(RecordStore& records, const char* name, size_t minimum, size_t maximum)
        : records_(records), name_(name), minimum_(minimum), maximum_(maximum) {}
    ReadResult read(uint8_t* output, size_t capacity, size_t& size) override {
        return records_.read(name_, output, capacity, size);
    }
    bool replace(const uint8_t* input, size_t size) override {
        return size >= minimum_ && size <= maximum_ && records_.write(name_, input, size);
    }

private:
    RecordStore& records_;
    const char* name_;
    size_t minimum_, maximum_;
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
// Durable protocol state over records v2 (docs/persistence.md). A device with only the
// v1 snapshot is migrated on its first mount.
class PersistentStore final : public CounterStore, public Journal {
public:
    PersistentStore(RecordStore&, Role, uint64_t device);
    // One owner per backing store. Duplicating cached counters can reuse GCM nonces.
    PersistentStore(const PersistentStore&) = delete;
    PersistentStore& operator=(const PersistentStore&) = delete;
    PersistentStore(PersistentStore&&) = delete;
    PersistentStore& operator=(PersistentStore&&) = delete;
    bool mount();
    bool healthy() const { return health_ == Health::Ready; }
    Health health() const { return health_; }
    uint64_t device() const { return device_; }
    Role role() const { return role_; }
    uint64_t network() const { return registry_.network; }
    uint64_t receiver() const { return registry_.receiver; }
    uint16_t profile() const { return registry_.profile; }
    size_t queued() const { return size_t(tail_ - head_); }
    Result prepare(uint64_t network, uint64_t receiver, uint64_t node, uint64_t generation,
                   const Key&, uint16_t profile);
    Result activate(uint64_t node, uint64_t generation);
    // Receiver, radio pairing: activates without revoking the node's previous generation,
    // which stays valid until the node's first sample under the new one (commit revokes
    // it then). A previous generation that never received a sample is revoked now.
    Result activateAlongside(uint64_t node, uint64_t generation);
    Result revoke(uint64_t node, uint64_t generation);
    bool info(uint64_t node, uint64_t generation, EnrollmentInfo&) const;
    // Copies up to capacity occupied enrollments in slot order; returns how many.
    size_t list(EnrollmentInfo* output, size_t capacity) const;
    // Slots a new generation can use: empty ones plus revoked ones without queued samples,
    // which prepare() retires to make room.
    size_t freeSlots() const;
    bool binding(uint64_t node, Binding&) const;
    // Every active binding of a node: two on a receiver while a re-pairing is pending.
    size_t bindings(uint64_t node, Binding* output, size_t capacity) const;
    bool reserve(const Binding&, uint64_t&) override;
    bool load(const Binding&, Receipt&) override;
    Result commit(const Binding&, uint64_t expectedCounter, const Receipt&) override;
    // Reads the queue front from storage; false when empty or unreadable (then unhealthy).
    bool peek(QueuedSample&);
    Result forwarded(uint64_t node, uint64_t generation, uint64_t counter);
    // Leaves the network: every enrollment is retired and the registry emptied, so the
    // device can join another network with fresh credentials. Conflict while samples are
    // queued unless discardQueue.
    Result reset(bool discardQueue);

private:
    RecordStore& records_;
    Role role_;
    uint64_t device_;
    Health health_ = Health::Unmounted;
    records::Registry registry_{}, next_{};
    records::RetiredList retired_{}, nextRetired_{};
    std::array<records::ReceiptRecord, BindingCapacity> receipts_{};
    std::array<uint16_t, BindingCapacity> queuedBySlot_{};
    uint64_t head_ = 0, tail_ = 0;
    std::array<uint8_t, records::BufferCapacity> buffer_{};
    int find(uint64_t node, uint64_t generation) const;
    int authorized(const Binding&) const;
    void clear();
    Health load();
    Health loadReceiver();
    Health migrate();
    bool fail(Health);
    bool saveRegistry();
    bool saveRetired();
    bool saveHead(uint64_t sequence);
    int reclaimable() const;
    Health readFront(records::QueueRecord&);
};
} // namespace cajui
