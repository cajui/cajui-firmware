#pragma once
#include "cajui_application.h"
#include "cajui_storage.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
// Receiver uplink settings. Stored in their own blob, never in the protocol snapshot.
constexpr size_t SsidCapacity = 32, WifiPasswordCapacity = 64, HostCapacity = 64,
                 UsernameCapacity = 64, MqttPasswordCapacity = 64;
constexpr size_t MinWifiPassword = 8;
struct UplinkConfig {
    char ssid[SsidCapacity + 1]{};
    char wifiPassword[WifiPasswordCapacity + 1]{};
    char host[HostCapacity + 1]{};
    uint16_t port = 0;
    // The MQTT username is also the telemetry source_id and topic namespace, so a
    // broker ACL granting each user its own namespace authorizes it by construction.
    char username[UsernameCapacity + 1]{};
    char password[MqttPasswordCapacity + 1]{};
};
// Uplink blob v1: magic 4, version 1, five length-prefixed strings, port 2, CRC32 4.
constexpr size_t MinUplinkSize = 4 + 1 + 5 + 2 + 4;
constexpr size_t UplinkBlobCapacity = MinUplinkSize + SsidCapacity + WifiPasswordCapacity +
                                      HostCapacity + UsernameCapacity + MqttPasswordCapacity;
// Open networks and hidden-length secrets are not supported: every field is required.
bool validUplink(const UplinkConfig&);
bool validIdentity(const char*);
// IPv4 address or host name characters, 1..HostCapacity bytes.
bool validHost(const char*);
// Decimal 1..65535 without sign, spaces or leading text.
bool parsePort(const char*, uint16_t&);
void wipe(UplinkConfig&);
// Stored values are validated again on load; a false/Error result disables forwarding.
ReadResult loadUplink(AtomicBlob&, UplinkConfig&);
bool saveUplink(AtomicBlob&, const UplinkConfig&);

// Cajuí Central MQTT telemetry contract, version 1:
// topic telemetry/v1/<source_id>/<device_id>/samples, QoS 1, retain false.
constexpr size_t TopicCapacity = 128, PayloadCapacity = 1536;
bool formatTopic(const char* source, uint64_t device, char* output, size_t capacity);
// sample_id is "<generation>.<counter>": stable across retries and unique per acquisition
// for a credential. measured_at is omitted because the receiver does not know it.
bool formatSample(const char* source, const QueuedSample&, char* output, size_t capacity,
                  size_t& size);

class Publisher {
public:
    virtual ~Publisher() = default;
    virtual bool connected() = 0;
    // Queues a QoS 1, non-retained publication without blocking the caller.
    // Returns a positive message id, or a value <= 0 when not accepted.
    virtual int publish(const char* topic, const char* payload, size_t size) = 0;
    // Pops one message id acknowledged by the broker (PUBACK); false when none is pending.
    virtual bool acknowledged(int& id) = 0;
};
enum class ForwardState { Idle, Waiting, Failed };
// Publishes the queue front and removes it only after the broker acknowledged that
// exact publication. A lost PUBACK republishes the same sample_id; Central deduplicates.
// PUBACK is a broker boundary: it does not prove that Central stored the sample.
class Forwarder final {
public:
    static constexpr uint32_t AckTimeoutMs = 15000, RetryDelayMs = 5000;
    Forwarder(Publisher&, Clock&, PersistentStore&, const char* source);
    Forwarder(const Forwarder&) = delete;
    Forwarder& operator=(const Forwarder&) = delete;
    // quiet=false defers all work, including flash writes, while the radio needs the loop.
    void poll(bool quiet);
    // Switches to a new broker identity. An in-flight publication is abandoned; the
    // sample stays queued and is republished under the new source.
    bool setSource(const char* source);
    ForwardState state() const { return state_; }
    uint32_t forwarded() const { return forwarded_; }
    uint32_t retries() const { return retries_; }

private:
    Publisher& publisher_;
    Clock& clock_;
    PersistentStore& store_;
    char source_[UsernameCapacity + 1]{};
    char topic_[TopicCapacity]{};
    char payload_[PayloadCapacity]{};
    ForwardState state_ = ForwardState::Idle;
    uint64_t node_ = 0, generation_ = 0, counter_ = 0;
    int message_ = 0;
    uint32_t sentAt_ = 0, retryAt_ = 0, forwarded_ = 0, retries_ = 0;
    bool delayed_ = false;
    void retryLater(uint32_t now);
};
} // namespace cajui
