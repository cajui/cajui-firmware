// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_manage.h"
#include "cajui_storage.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
// Home Assistant MQTT Discovery, published by the receiver so an installation without
// Cajuí Central sets itself up too (docs/home-assistant.md). Topics are
// homeassistant/sensor/<source_id>/<object_id>/config: the source is Discovery's optional
// node level, so a broker ACL can confine each receiver to its own configurations.
enum class Entity : uint8_t { Temperature, Humidity, Rssi, Snr, WifiRssi, QueueDepth, Uptime };
constexpr size_t NodeEntities = 4, ReceiverEntities = 3, DiscoveryCapacity = 1024;
struct DiscoveryItem {
    uint64_t device = 0; // A transmitter's node ID, or the receiver's own ID.
    uint16_t sensor = 0; // Sensor of a transmitter measurement; unused otherwise.
    Entity entity = Entity::Temperature;
    uint32_t intervalS = 0; // The transmitter's expected interval, for expire_after.
};
bool formatDiscoveryTopic(const char* source, const DiscoveryItem&, char* output, size_t capacity);
bool formatDiscovery(const char* source, uint64_t receiver, const DiscoveryItem&, char* output,
                     size_t capacity, size_t& size);

// Publishes one retained configuration per poll: the receiver's diagnostics after each
// connection, and each transmitter entity once it appears in a forwarded sample (and again
// after each connection or when its interval changes).
class DiscoveryReporter final : public SampleObserver {
public:
    DiscoveryReporter(StatePublisher&, uint64_t receiver, const char* source);
    DiscoveryReporter(const DiscoveryReporter&) = delete;
    DiscoveryReporter& operator=(const DiscoveryReporter&) = delete;
    bool setSource(const char* source);
    void pause() { valid_ = false; }
    // Learns the entities of a sample the broker acknowledged.
    void sampleForwarded(const QueuedSample&) override;
    // True when it published (the caller's one client-lock wait for this loop pass).
    bool poll();

private:
    struct Node {
        uint64_t node = 0;
        uint16_t sensor = 0;
        uint32_t intervalS = 0;
        uint8_t known = 0, published = 0; // Bits of Entity::Temperature..Snr.
    };
    StatePublisher& publisher_;
    uint64_t receiver_;
    char source_[UsernameCapacity + 1]{};
    char topic_[TopicCapacity]{};
    char payload_[DiscoveryCapacity]{};
    Node nodes_[BindingCapacity]{};
    uint8_t receiverPublished_ = 0;
    uint32_t session_ = 0;
    bool valid_ = false;
    bool publish(const DiscoveryItem&);
};
} // namespace cajui
