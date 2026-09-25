// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_uplink.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mqtt_client.h>
#include <atomic>

namespace board {
// Wi-Fi station plus the ESP-IDF MQTT client. Plain TCP only: use a trusted network
// until broker TLS is provisioned. MQTT 3.1.1 cannot report an ACL-denied publication.
// The setup page replaces the client from its own task while the radio loop publishes:
// an internal mutex guards the client, and the loop's calls never wait for that mutex.
class MqttUplink final : public cajui::Publisher {
public:
    MqttUplink() = default;
    // Creates the mutex and acknowledgement queue; call from setup() before other tasks.
    bool prepare();
    MqttUplink(const MqttUplink&) = delete;
    MqttUplink& operator=(const MqttUplink&) = delete;
    // Station mode plus MQTT. Copies what it needs; the caller may wipe the configuration.
    bool begin(const cajui::UplinkConfig&, uint64_t device);
    // Joins a network in the current Wi-Fi mode (station or access point plus station).
    static void startWifi(const char* ssid, const char* password);
    // Replaces any running MQTT client with one for these settings.
    bool startMqtt(const cajui::UplinkConfig&, uint64_t device);
    bool connected() override { return online_.load(); }
    int publish(const char* topic, const char* payload, size_t size) override;
    bool acknowledged(int& id) override;
    static bool wifiConnected();

private:
    esp_mqtt_client_handle_t client_ = nullptr;
    QueueHandle_t acks_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    std::atomic<bool> online_{false};
    void stopMqtt();
    bool restart(const cajui::UplinkConfig&, uint64_t device);
    static void onEvent(void* self, esp_event_base_t, int32_t event, void* data);
};
} // namespace board
