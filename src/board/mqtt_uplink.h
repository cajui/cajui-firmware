// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_command.h"
#include "cajui_manage.h"
#include "cajui_uplink.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mqtt_client.h>
#include <atomic>

namespace board {
// Wi-Fi station plus the ESP-IDF MQTT client. Plain TCP only: use a trusted network
// until broker TLS is provisioned. MQTT 3.1.1 cannot report an ACL-denied publication.
// The radio loop only copies publications into an outbox; a sender task hands them to the
// client, whose internal lock can be held for a network operation. So the loop never waits
// for the network and the receiver keeps acknowledging within the node's ACK window. The
// setup page replaces the client from its own task; an internal mutex guards the client.
//
// Management channel (docs/management-v1.md): the client's last will marks the receiver
// offline, and "online" is published on each connection. A broker that refuses the
// connection as not authorized may be refusing the will topic, so the client reconnects
// without the will and without management publications until the next restart; telemetry
// keeps flowing either way.
class MqttUplink final : public cajui::Publisher, public cajui::StatePublisher {
public:
    MqttUplink() = default;
    ~MqttUplink() override;
    // Creates the mutex, acknowledgement queue and restart task; call from setup() before
    // other tasks.
    bool prepare();
    MqttUplink(const MqttUplink&) = delete;
    MqttUplink& operator=(const MqttUplink&) = delete;
    // Station mode plus MQTT. Copies what it needs; the caller may wipe the configuration.
    bool begin(const cajui::UplinkConfig&, uint64_t device);
    // Joins a network in the current Wi-Fi mode (station or access point plus station).
    static void startWifi(const char* ssid, const char* password);
    // Replaces any running MQTT client with one for these settings. The old connection
    // first publishes its retained "offline", since a clean disconnect fires no will.
    bool startMqtt(const cajui::UplinkConfig&, uint64_t device);
    // Queues a retained "offline" before a deliberate restart; the caller then gives the
    // client a moment to send it.
    void announceOffline();
    // Stop and forget MQTT before trying another Wi-Fi network.
    void suspend();
    bool connected() override { return online_.load(); }
    int publish(const char* topic, const char* payload, size_t size) override;
    bool acknowledged(int& id) override;
    bool ready() override { return online_.load() && managed_.load(); }
    uint32_t session() override { return session_.load(); }
    bool publishRetained(const char* topic, const char* payload, size_t size) override;
    // A command received on this receiver's commands topic, copied out of the client's task.
    struct Incoming {
        uint64_t device;
        uint32_t receivedAt;
        uint32_t generation; // Of the client that received it; see publishResult.
        size_t size;
        char payload[cajui::CommandPayloadCapacity + 1];
    };
    // Pops one received command without waiting; false when none is queued.
    bool nextCommand(Incoming&);
    // Queues a non-retained QoS 1 result; false when not accepted. A result for a command
    // received by an earlier client (other settings) is dropped, never sent under the new
    // identity.
    bool publishResult(uint64_t device, const char* payload, uint32_t generation);
    uint32_t generation() const { return generation_.load(); }
    static bool wifiConnected();

private:
    static constexpr size_t StateIds = 8;
    esp_mqtt_client_handle_t client_ = nullptr;
    // One publication waiting for the sender task. Samples keep their local ID so the
    // broker's PUBACK can be matched to what the forwarder published.
    enum class Kind : uint8_t { Sample, State, Result, Offline };
    struct Outgoing {
        Kind kind;
        bool retain;
        int local;
        uint32_t generation; // Of the client it was meant for; dropped after a replacement.
        uint64_t device;     // Results: the topic's device, formatted by the sender.
        size_t size;
        char topic[cajui::TopicCapacity];
        char payload[cajui::PayloadCapacity + 1];
    };
    static constexpr size_t Mappings = 4;
    QueueHandle_t acks_ = nullptr, commands_ = nullptr, outbox_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t restarter_ = nullptr, sender_ = nullptr;
    Outgoing staging_{}; // Filled by the radio loop only.
    Outgoing sending_{}; // Used by the sender task only.
    std::atomic<int> nextLocal_{0};
    // Client message ID -> local sample ID, for the PUBACK handed to the forwarder.
    std::atomic<int> messageIds_[Mappings]{}, localIds_[Mappings]{};
    std::atomic<size_t> nextMapping_{0};
    std::atomic<bool> online_{false}, managed_{true};
    std::atomic<uint32_t> session_{0}, generation_{0};
    // Message IDs of management publications, whose PUBACKs must not reach the forwarder.
    std::atomic<int> stateIds_[StateIds]{};
    std::atomic<size_t> nextStateId_{0};
    // Kept for the restart without the will; wiped on destruction.
    cajui::UplinkConfig settings_{};
    uint64_t device_ = 0;
    char availability_[cajui::TopicCapacity]{}, commandFilter_[cajui::TopicCapacity]{};
    void stopMqtt();
    bool restart(const cajui::UplinkConfig&, uint64_t device);
    bool stage(Kind, const char* topic, const char* payload, size_t size, bool retain, int local,
               uint64_t device, uint32_t generation);
    void send();
    static void senderTask(void* self);
    void rememberStateId(int id);
    void receive(const esp_mqtt_event_t&);
    bool takeStateId(int id);
    static void restartTask(void* self);
    static void onEvent(void* self, esp_event_base_t, int32_t event, void* data);
};
} // namespace board
