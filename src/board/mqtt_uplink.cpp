// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "mqtt_uplink.h"
#include <WiFi.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace board {
namespace {
constexpr UBaseType_t AckDepth = 8; // Overflow only delays removal until the retry timeout.
// A full queue drops a command; its sender gets no result and treats it as not delivered.
constexpr UBaseType_t CommandDepth = 4;
// The client's own lock can be held for a network operation up to this timeout, and the
// radio loop enqueues under that lock: keep it well below the 5-second task watchdog.
constexpr int KeepaliveSeconds = 60, ReconnectMs = 5000, NetworkTimeoutMs = 2500;
constexpr int QoS = 1, Retain = 0;
constexpr uint32_t RestartStack = 4096;
constexpr int BufferBytes = 2048;
const char Online[] = "online", Offline[] = "offline";
}
MqttUplink::~MqttUplink() {
    cajui::wipe(settings_);
}
bool MqttUplink::prepare() {
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!acks_) acks_ = xQueueCreate(AckDepth, sizeof(int));
    if (!commands_) commands_ = xQueueCreate(CommandDepth, sizeof(Incoming));
    // Replacing the client can wait for its network task, which neither the radio loop nor
    // the client's own event handler may do.
    if (!restarter_)
        xTaskCreate(restartTask, "cajui-mqtt", RestartStack, this, tskIDLE_PRIORITY + 1,
                    &restarter_);
    return mutex_ && acks_ && commands_ && restarter_;
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
        // A clean disconnect fires no will: say it ourselves while the old identity is
        // still connected. esp_mqtt_client_publish sends at once, within the network timeout.
        if (online_.load() && managed_.load())
            esp_mqtt_client_publish(client_, availability_, Offline, 0, QoS, 1);
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    }
    online_.store(false);
    // Message IDs restart with a new client; stale acknowledgements must not match them.
    if (acks_) xQueueReset(acks_);
    // Commands addressed through the old identity must not run under the new one.
    if (commands_) xQueueReset(commands_);
    for (auto& id : stateIds_) id.store(0);
}
bool MqttUplink::startMqtt(const cajui::UplinkConfig& config, uint64_t device) {
    if (!cajui::validUplink(config) || !prepare()) return false;
    // Stopping a client can wait for its network task; only this task waits here.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // The old connection says "offline" only if it was allowed management publications;
    // new settings then deserve a new attempt with the will.
    stopMqtt();
    managed_.store(true);
    const bool started = restart(config, device);
    xSemaphoreGive(mutex_);
    return started;
}
bool MqttUplink::restart(const cajui::UplinkConfig& config, uint64_t device) {
    stopMqtt();
    generation_.fetch_add(1);
    if (&config != &settings_) settings_ = config;
    device_ = device;
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
    // The default 1024-byte buffer is shared by topic and payload; a Discovery
    // configuration or a full sample needs more.
    settings.buffer_size = BufferBytes;
    settings.user_context = this;
    if (!cajui::formatManageTopic(config.username, device, cajui::ManageTopic::Availability,
                                  availability_, sizeof(availability_)) ||
        std::snprintf(commandFilter_, sizeof(commandFilter_), "manage/v1/%s/+/commands",
                      config.username) >= int(sizeof(commandFilter_)))
        managed_.store(false);
    if (managed_.load()) {
        settings.lwt_topic = availability_;
        settings.lwt_msg = Offline;
        settings.lwt_qos = QoS;
        settings.lwt_retain = 1;
    }
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
void MqttUplink::restartTask(void* self) {
    auto* uplink = static_cast<MqttUplink*>(self);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(uplink->mutex_, portMAX_DELAY);
        if (uplink->client_) uplink->restart(uplink->settings_, uplink->device_);
        xSemaphoreGive(uplink->mutex_);
    }
}
void MqttUplink::onEvent(void* self, esp_event_base_t, int32_t event, void* data) {
    auto* uplink = static_cast<MqttUplink*>(self);
    const auto* details = static_cast<esp_mqtt_event_handle_t>(data);
    switch (esp_mqtt_event_id_t(event)) {
    case MQTT_EVENT_CONNECTED:
        // Runs in the client's task, whose API lock is recursive: enqueueing here is safe.
        if (uplink->managed_.load()) {
            uplink->rememberStateId(esp_mqtt_client_enqueue(details->client, uplink->availability_,
                                                            Online, 0, QoS, 1, true));
            // A clean session keeps no subscription: subscribe on every connection.
            esp_mqtt_client_subscribe(details->client, uplink->commandFilter_, QoS);
        }
        uplink->session_.fetch_add(1);
        uplink->online_.store(true);
        break;
    case MQTT_EVENT_DISCONNECTED: uplink->online_.store(false); break;
    case MQTT_EVENT_PUBLISHED:
        if (!uplink->takeStateId(details->msg_id)) xQueueSend(uplink->acks_, &details->msg_id, 0);
        break;
    case MQTT_EVENT_DATA: uplink->receive(*details); break;
    case MQTT_EVENT_ERROR:
        if (uplink->managed_.load() && details->error_handle &&
            details->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            details->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
            uplink->managed_.store(false);
            xTaskNotifyGive(uplink->restarter_);
        }
        break;
    default: break;
    }
}
void MqttUplink::rememberStateId(int id) {
    if (id <= 0) return;
    // A slot overwritten before its PUBACK only lets one stray ID reach the forwarder,
    // which ignores IDs it did not publish.
    stateIds_[nextStateId_.fetch_add(1) % StateIds].store(id);
}
bool MqttUplink::takeStateId(int id) {
    for (auto& slot : stateIds_) {
        int expected = id;
        if (id > 0 && slot.compare_exchange_strong(expected, 0)) return true;
    }
    return false;
}
// Runs in the client's task: copy and queue only. A retained command would run again after
// every reconnection, and a fragmented one exceeds the contract's 512 bytes.
void MqttUplink::receive(const esp_mqtt_event_t& event) {
    if (!managed_.load() || event.retain || event.current_data_offset != 0 ||
        event.data_len != event.total_data_len || event.data_len < 0 ||
        size_t(event.data_len) > cajui::CommandPayloadCapacity || event.topic_len <= 0 ||
        size_t(event.topic_len) >= cajui::TopicCapacity)
        return;
    char topic[cajui::TopicCapacity]{};
    std::memcpy(topic, event.topic, size_t(event.topic_len));
    Incoming incoming{};
    if (!cajui::parseCommandTopic(topic, settings_.username, incoming.device)) return;
    incoming.receivedAt = millis();
    incoming.generation = generation_.load();
    incoming.size = size_t(event.data_len);
    std::memcpy(incoming.payload, event.data, incoming.size);
    xQueueSend(commands_, &incoming, 0);
}
bool MqttUplink::nextCommand(Incoming& incoming) {
    return commands_ && xQueueReceive(commands_, &incoming, 0) == pdTRUE;
}
bool MqttUplink::publishResult(uint64_t device, const char* payload, uint32_t generation) {
    // settings_ changes only under the mutex, while the client is replaced.
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return false;
    char topic[cajui::TopicCapacity]{};
    const int id =
        generation == generation_.load() && client_ && online_.load() && managed_.load() &&
                cajui::formatManageTopic(settings_.username, device, cajui::ManageTopic::Results,
                                         topic, sizeof(topic))
            ? esp_mqtt_client_enqueue(client_, topic, payload, 0, QoS, 0, true)
            : -1;
    rememberStateId(id);
    xSemaphoreGive(mutex_);
    return id > 0;
}
int MqttUplink::enqueueRetained(const char* topic, const char* payload) {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return -1;
    const int id = client_ && online_.load() && managed_.load()
                       ? esp_mqtt_client_enqueue(client_, topic, payload, 0, QoS, 1, true)
                       : -1;
    rememberStateId(id);
    xSemaphoreGive(mutex_);
    return id;
}
bool MqttUplink::publishRetained(const char* topic, const char* payload, size_t size) {
    // The client computes the length of a NUL-terminated payload itself.
    if (std::strlen(payload) != size) return false;
    return enqueueRetained(topic, payload) > 0;
}
void MqttUplink::announceOffline() {
    enqueueRetained(availability_, Offline);
}
int MqttUplink::publish(const char* topic, const char* payload, size_t size) {
    // Enqueue instead of publish: the MQTT task performs network I/O. The loop can still
    // wait for the client's lock, at most the network timeout. The outbox copies the payload. While
    // the client is being replaced the publication is refused and retried later.
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return -1;
    const int id =
        client_ ? esp_mqtt_client_enqueue(client_, topic, payload, int(size), QoS, Retain, true)
                : -1;
    xSemaphoreGive(mutex_);
    return id;
}
bool MqttUplink::acknowledged(int& id) {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return false;
    const bool popped = acks_ && xQueueReceive(acks_, &id, 0) == pdTRUE;
    xSemaphoreGive(mutex_);
    return popped;
}
bool MqttUplink::wifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}
} // namespace board
#endif
