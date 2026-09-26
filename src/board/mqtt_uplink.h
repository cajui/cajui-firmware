// SPDX-License-Identifier: Apache-2.0
#pragma once
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
// The setup page replaces the client from its own task while the radio loop publishes:
// an internal mutex guards the client, and the loop's calls never wait for that mutex.
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
    bool connected() override { return online_.load(); }
    int publish(const char* topic, const char* payload, size_t size) override;
    bool acknowledged(int& id) override;
    bool ready() override { return online_.load() && managed_.load(); }
    uint32_t session() override { return session_.load(); }
    bool publishRetained(const char* topic, const char* payload, size_t size) override;
    static bool wifiConnected();

private:
    static constexpr size_t StateIds = 8;
    esp_mqtt_client_handle_t client_ = nullptr;
    QueueHandle_t acks_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t restarter_ = nullptr;
    std::atomic<bool> online_{false}, managed_{true};
    std::atomic<uint32_t> session_{0};
    // Message IDs of management publications, whose PUBACKs must not reach the forwarder.
    std::atomic<int> stateIds_[StateIds]{};
    std::atomic<size_t> nextStateId_{0};
    // Kept for the restart without the will; wiped on destruction.
    cajui::UplinkConfig settings_{};
    uint64_t device_ = 0;
    char availability_[cajui::TopicCapacity]{};
    void stopMqtt();
    bool restart(const cajui::UplinkConfig&, uint64_t device);
    int enqueueRetained(const char* topic, const char* payload);
    void rememberStateId(int id);
    bool takeStateId(int id);
    static void restartTask(void* self);
    static void onEvent(void* self, esp_event_base_t, int32_t event, void* data);
};
} // namespace board
