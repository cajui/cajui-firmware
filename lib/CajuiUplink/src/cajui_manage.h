// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_pairing.h"
#include "cajui_uplink.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
// MQTT management channel v1, docs/management-v1.md. This part publishes retained device
// state; commands are not implemented yet.
enum class ManageTopic { Availability, State, Commands, Results };
bool formatManageTopic(const char* source, uint64_t device, ManageTopic, char* output,
                       size_t capacity);

// What the receiver knows about itself. Null strings and *Known=false are unknown values,
// published as null, never as zero.
struct ReceiverStatus {
    uint64_t device = 0;
    const char* model = nullptr;
    uint32_t firmwareVersion = 0; // major*10000 + minor*100 + patch; 0 is a local build.
    const char* slot = nullptr;
    const char* firmwareState = nullptr;
    uint16_t profile = 0;
    int8_t powerDbm = 0;
    uint32_t uptimeS = 0;
    const char* resetReason = nullptr;
    bool wifiKnown = false;
    int16_t wifiRssiDbm = 0;
    size_t queued = 0, queueCapacity = 0;
    uint32_t published = 0, retries = 0;
    bool pairingOpen = false;
    uint32_t pairingRemainingS = 0;
    const Candidate* requests = nullptr;
    size_t requestCount = 0;
};
// What the receiver knows about one transmitter: its binding and the last frame it
// accepted since starting. The transmitter's own model, firmware and parameters are not
// carried by the radio protocol yet and are published as null.
// Active: an active enrollment. Pending: only one prepared over USB, not yet activated.
enum class NodeBinding { Active, Pending, Revoked };
struct NodeStatus {
    uint64_t node = 0, receiver = 0;
    NodeBinding binding = NodeBinding::Revoked;
    bool frameKnown = false;
    uint64_t counter = 0;
    Link link{};
    uint32_t receiverUptimeS = 0;
};
constexpr size_t StateCapacity = 1024;
bool formatReceiverState(const char* source, const ReceiverStatus&, char* output, size_t capacity,
                         size_t& size);
bool formatNodeState(const char* source, const NodeStatus&, char* output, size_t capacity,
                     size_t& size);

// Retained QoS 1 publications, implemented by the board MQTT client.
class StatePublisher {
public:
    virtual ~StatePublisher() = default;
    // Connected, and the broker accepted the management topics (docs/management-v1.md).
    virtual bool ready() = 0;
    // Counts broker connections: a new value means retained state must be sent again.
    virtual uint32_t session() = 0;
    // Queues without blocking; false when not accepted (retried later).
    virtual bool publishRetained(const char* topic, const char* payload, size_t size) = 0;
};
// Current values, read under the caller's lock.
class StateSource {
public:
    virtual ~StateSource() = default;
    virtual void receiverStatus(ReceiverStatus&) = 0;
    // Distinct node IDs with an active or revoked binding, up to capacity.
    virtual size_t nodes(uint64_t* output, size_t capacity) = 0;
    virtual bool nodeStatus(uint64_t node, NodeStatus&) = 0;
};

// Decides when to publish state. The receiver's state goes out after each connection, as
// soon as a discrete field changes (firmware, radio, pairing), and otherwise every
// CounterIntervalMs, since its counters (uptime first) always move. A node's state goes out after
// each connection and whenever nodeChanged() reports it. One publication per poll keeps the loop's
// work bounded; a refused one is retried after RetryMs.
class StateReporter final {
public:
    static constexpr uint32_t CounterIntervalMs = 60000, RetryMs = 1000;
    StateReporter(StatePublisher&, StateSource&, Clock&, const char* source);
    StateReporter(const StateReporter&) = delete;
    StateReporter& operator=(const StateReporter&) = delete;
    // A new broker identity: everything is published again under it.
    bool setSource(const char* source);
    void nodeChanged(uint64_t node);
    void poll();

private:
    struct Discrete {
        uint32_t firmwareVersion = 0;
        const char* slot = nullptr;
        const char* firmwareState = nullptr;
        uint16_t profile = 0;
        int8_t powerDbm = 0;
        bool pairingOpen = false;
        size_t requestCount = 0;
        uint64_t requests[MaxCandidates]{};
        bool conflicts[MaxCandidates]{};
    };
    StatePublisher& publisher_;
    StateSource& source_;
    Clock& clock_;
    char name_[UsernameCapacity + 1]{};
    char topic_[TopicCapacity]{};
    char payload_[StateCapacity]{};
    uint32_t session_ = 0, receiverAt_ = 0, retryAt_ = 0;
    bool valid_ = false, receiverDue_ = false, delayed_ = false;
    Discrete discrete_{};
    uint64_t dirty_[BindingCapacity]{};
    size_t dirtyCount_ = 0;
    static bool same(const Discrete&, const Discrete&);
    void markAll();
    bool publishReceiver(const ReceiverStatus&);
    bool publishNode(uint64_t node);
};
} // namespace cajui
