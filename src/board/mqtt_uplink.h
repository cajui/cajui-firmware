#pragma once
#include "cajui_uplink.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>
#include <atomic>

namespace board {
// Wi-Fi station plus the ESP-IDF MQTT client. Plain TCP only: use a trusted network
// until broker TLS is provisioned. MQTT 3.1.1 cannot report an ACL-denied publication.
class MqttUplink final : public cajui::Publisher {
public:
    MqttUplink() = default;
    MqttUplink(const MqttUplink&) = delete;
    MqttUplink& operator=(const MqttUplink&) = delete;
    // Copies what it needs; the caller may wipe the configuration afterwards.
    bool begin(const cajui::UplinkConfig&, uint64_t device);
    bool connected() override { return online_.load(); }
    int publish(const char* topic, const char* payload, size_t size) override;
    bool acknowledged(int& id) override;
    bool wifiConnected() const;

private:
    esp_mqtt_client_handle_t client_ = nullptr;
    QueueHandle_t acks_ = nullptr;
    std::atomic<bool> online_{false};
    static void onEvent(void* self, esp_event_base_t, int32_t event, void* data);
};
} // namespace board
