#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "mqtt_uplink.h"
#include <WiFi.h>
#include <cinttypes>
#include <cstdio>

namespace board {
namespace {
constexpr UBaseType_t AckDepth = 8; // Overflow only delays removal until the retry timeout.
constexpr int KeepaliveSeconds = 60, ReconnectMs = 5000, NetworkTimeoutMs = 10000;
constexpr int QoS = 1, Retain = 0;
}
bool MqttUplink::begin(const cajui::UplinkConfig& config, uint64_t device) {
    if (!cajui::validUplink(config)) return false;
    WiFi.mode(WIFI_STA);
    startWifi(config.ssid, config.wifiPassword);
    return startMqtt(config, device);
}
void MqttUplink::startWifi(const char* ssid, const char* password) {
    // Keep Wi-Fi credentials out of the default NVS partition; they live in the uplink blob.
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
}
void MqttUplink::stopMqtt() {
    if (client_) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    online_.store(false);
    // Message IDs restart with a new client; stale acknowledgements must not match them.
    if (acks_) xQueueReset(acks_);
}
bool MqttUplink::startMqtt(const cajui::UplinkConfig& config, uint64_t device) {
    if (!cajui::validUplink(config)) return false;
    if (!acks_) acks_ = xQueueCreate(AckDepth, sizeof(int));
    if (!acks_) return false;
    stopMqtt();
    char clientId[sizeof("cajui-rx-") + 16]{};
    std::snprintf(clientId, sizeof(clientId), "cajui-rx-%016" PRIx64, device);
    esp_mqtt_client_config_t settings{};
    settings.host = config.host;
    settings.port = config.port;
    settings.transport = MQTT_TRANSPORT_OVER_TCP;
    settings.client_id = clientId;
    settings.username = config.username;
    settings.password = config.password;
    settings.keepalive = KeepaliveSeconds;
    settings.reconnect_timeout_ms = ReconnectMs;
    settings.network_timeout_ms = NetworkTimeoutMs;
    settings.user_context = this;
    // esp_mqtt_client_init copies every string it keeps.
    client_ = esp_mqtt_client_init(&settings);
    if (!client_) return false;
    if (esp_mqtt_client_register_event(client_, esp_mqtt_event_id_t(ESP_EVENT_ANY_ID), onEvent,
                                       this) != ESP_OK ||
        esp_mqtt_client_start(client_) != ESP_OK) {
        stopMqtt();
        return false;
    }
    return true;
}
void MqttUplink::onEvent(void* self, esp_event_base_t, int32_t event, void* data) {
    auto* uplink = static_cast<MqttUplink*>(self);
    const auto* details = static_cast<esp_mqtt_event_handle_t>(data);
    switch (esp_mqtt_event_id_t(event)) {
    case MQTT_EVENT_CONNECTED: uplink->online_.store(true); break;
    case MQTT_EVENT_DISCONNECTED: uplink->online_.store(false); break;
    case MQTT_EVENT_PUBLISHED: xQueueSend(uplink->acks_, &details->msg_id, 0); break;
    default: break;
    }
}
int MqttUplink::publish(const char* topic, const char* payload, size_t size) {
    // Enqueue instead of publish: the MQTT task performs network I/O, so the radio loop
    // never blocks on a slow broker. The outbox copies the payload.
    return client_ ? esp_mqtt_client_enqueue(client_, topic, payload, int(size), QoS, Retain, true)
                   : -1;
}
bool MqttUplink::acknowledged(int& id) {
    return acks_ && xQueueReceive(acks_, &id, 0) == pdTRUE;
}
bool MqttUplink::wifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}
} // namespace board
#endif
