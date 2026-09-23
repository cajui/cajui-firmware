#pragma once
#include "cajui_protocol.h"

namespace cajui {
constexpr size_t BindingCapacity = 16, QueueCapacity = 128;
constexpr size_t SnapshotSize = 42 + BindingCapacity * 186 + QueueCapacity * 138 + 4;
enum class Role : uint8_t { Transmitter = 1, Receiver = 2 };
enum class Enrollment : uint8_t { Empty = 0, Prepared = 1, Active = 2, Revoked = 3 };
enum class ReadResult { Ok, Missing, Error };
// Why the store refuses work; Ready is the only usable state.
enum class Health : uint8_t { Unmounted, Ready, Identity, ReadError, Corrupt, Format, Role, Device,
                              Invalid, WriteError };
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
    uint64_t node = 0, generation = 0, counter = 0;
};
struct QueuedSample {
    uint64_t node = 0, generation = 0, counter = 0;
    Data data{};
};
class PersistentStore final : public CounterStore, public Journal {
public:
    PersistentStore(AtomicBlob&, Role, uint64_t device);
    bool mount();
    bool healthy() const { return health_ == Health::Ready; }
    Health health() const { return health_; }
    uint64_t device() const { return device_; }
    Role role() const { return role_; }
    uint64_t network() const { return state_.network; }
    uint64_t receiver() const { return state_.receiver; }
    uint16_t profile() const { return state_.profile; }
    size_t queued() const { return state_.count; }
    Result prepare(uint64_t network, uint64_t receiver, uint64_t node,
                   uint64_t generation, const Key&, uint16_t profile);
    Result activate(uint64_t node, uint64_t generation);
    Result revoke(uint64_t node, uint64_t generation);
    bool info(uint64_t node, uint64_t generation, EnrollmentInfo&) const;
    bool binding(uint64_t node, Binding&) const;
    bool reserve(const Binding&, uint64_t&) override;
    bool load(const Binding&, Receipt&) override;
    Result commit(const Binding&, uint64_t expectedCounter, const Receipt&, const Data&) override;
    bool peek(QueuedSample&) const;
    Result forwarded(uint64_t node, uint64_t generation, uint64_t counter);
private:
    struct Entry {
        Enrollment state = Enrollment::Empty;
        uint64_t node = 0, generation = 0, counter = 0;
        Key key{};
        Receipt receipt{};
    };
    struct Queued { uint8_t entry = 0; Frame frame{}; };
    struct State {
        uint64_t revision = 0, network = 0, receiver = 0;
        uint16_t profile = 0, count = 0;
        std::array<Entry, BindingCapacity> entries{};
        std::array<Queued, QueueCapacity> queue{};
    };
    AtomicBlob& blob_;
    Role role_;
    uint64_t device_;
    Health health_ = Health::Unmounted;
    // Scratch space is part of the object, never a large MCU task-stack allocation.
    State state_{}, next_{};
    std::array<uint8_t, SnapshotSize> bytes_{};
    size_t bytesSize_ = 0;
    int find(uint64_t node, uint64_t generation) const;
    int authorized(const Binding&) const;
    Binding asBinding(const Entry&, uint64_t network) const;
    bool save();
    Health decode();
    void encode();
};
} // namespace cajui
