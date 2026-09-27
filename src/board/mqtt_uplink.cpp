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
// A full outbox refuses a publication; its producer retries later. Each entry is ~1.7 KB.
constexpr UBaseType_t OutboxDepth = 4;
// The client's own lock can be held for a network operation up to this timeout; only the
// sender, restart and setup tasks can wait for it, never the radio loop.
constexpr int KeepaliveSeconds = 60, ReconnectMs = 5000, NetworkTimeoutMs = 2500;
constexpr int QoS = 1, Retain = 0;
constexpr uint32_t RestartStack = 4096, SenderStack = 6144;
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
    if (!outbox_) outbox_ = xQueueCreate(OutboxDepth, sizeof(Outgoing));
    if (!sender_ && outbox_)
        xTaskCreate(senderTask, "cajui-mqtt-tx", SenderStack, this, tskIDLE_PRIORITY + 1, &sender_);
    // Replacing the client can wait for its network task, which neither the radio loop nor
    // the client's own event handler may do.
    if (!restarter_)
        xTaskCreate(restartTask, "cajui-mqtt", RestartStack, this, tskIDLE_PRIORITY + 1,
                    &restarter_);
    return mutex_ && acks_ && commands_ && outbox_ && restarter_ && sender_;
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
    // Commands addressed through the old identity must not run under the new one, and
    // publications meant for the old client are not sent through the new one.
    if (commands_) xQueueReset(commands_);
    if (outbox_) xQueueReset(outbox_);
    for (auto& id : stateIds_) id.store(0);
    for (auto& id : messageIds_) id.store(0);
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
    case MQTT_EVENT_PUBLISHED: {
        if (uplink->takeStateId(details->msg_id)) break;
        // A PUBACK that beats the sender's bookkeeping is unmatched: the forwarder then
        // republishes after its timeout and the consumer deduplicates the sample.
        for (size_t i = 0; i < Mappings; ++i) {
            int expected = details->msg_id;
            if (details->msg_id > 0 &&
                uplink->messageIds_[i].compare_exchange_strong(expected, 0)) {
                const int local = uplink->localIds_[i].load();
                xQueueSend(uplink->acks_, &local, 0);
                break;
            }
        }
        break;
    }
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
// The radio loop's side: copy and queue, never touch the client.
bool MqttUplink::stage(Kind kind, const char* topic, const char* payload, size_t size, bool retain,
                       int local, uint64_t device, uint32_t generation) {
    if (!outbox_ || size > cajui::PayloadCapacity) return false;
    staging_.kind = kind;
    staging_.retain = retain;
    staging_.local = local;
    staging_.generation = generation;
    staging_.device = device;
    staging_.size = size;
    staging_.topic[0] = 0;
    if (topic) {
        const size_t length = std::strlen(topic);
        if (length >= sizeof(staging_.topic)) return false;
        std::memcpy(staging_.topic, topic, length + 1);
    }
    std::memcpy(staging_.payload, payload, size);
    staging_.payload[size] = 0;
    return xQueueSend(outbox_, &staging_, 0) == pdTRUE;
}
bool MqttUplink::publishResult(uint64_t device, const char* payload, uint32_t generation) {
    return online_.load() && managed_.load() &&
           stage(Kind::Result, nullptr, payload, std::strlen(payload), false, 0, device,
                 generation);
}
bool MqttUplink::publishRetained(const char* topic, const char* payload, size_t size) {
    // The client computes the length of a NUL-terminated payload itself.
    return std::strlen(payload) == size && online_.load() && managed_.load() &&
           stage(Kind::State, topic, payload, size, true, 0, 0, generation_.load());
}
void MqttUplink::announceOffline() {
    stage(Kind::Offline, nullptr, Offline, sizeof(Offline) - 1, true, 0, 0, generation_.load());
}
int MqttUplink::publish(const char* topic, const char* payload, size_t size) {
    int local = nextLocal_.fetch_add(1) + 1;
    if (local <= 0) { // Wrapped: IDs stay positive, as the forwarder expects.
        nextLocal_.store(1);
        local = 1;
    }
    return stage(Kind::Sample, topic, payload, size, false, local, 0, generation_.load()) ? local
                                                                                          : -1;
}
// The sender task's side: it may wait for the client's lock, which the radio loop never does.
void MqttUplink::senderTask(void* self) {
    auto* uplink = static_cast<MqttUplink*>(self);
    for (;;)
        if (xQueueReceive(uplink->outbox_, &uplink->sending_, portMAX_DELAY) == pdTRUE)
            uplink->send();
}
void MqttUplink::send() {
    Outgoing& item = sending_;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool current = client_ && item.generation == generation_.load();
    const bool managed = online_.load() && managed_.load();
    int id = -1;
    if (current && item.kind == Kind::Sample) {
        id = esp_mqtt_client_enqueue(client_, item.topic, item.payload, int(item.size), QoS, Retain,
                                     true);
        if (id > 0) {
            const size_t slot = nextMapping_.fetch_add(1) % Mappings;
            localIds_[slot].store(item.local);
            messageIds_[slot].store(id);
        }
    } else if (current && managed) {
        char topic[cajui::TopicCapacity]{};
        const char* target = item.topic;
        if (item.kind == Kind::Offline)
            target = availability_;
        else if (item.kind == Kind::Result)
            target = cajui::formatManageTopic(settings_.username, item.device,
                                              cajui::ManageTopic::Results, topic, sizeof(topic))
                         ? topic
                         : nullptr;
        if (target)
            id = esp_mqtt_client_enqueue(client_, target, item.payload, 0, QoS, item.retain ? 1 : 0,
                                         true);
        rememberStateId(id);
    }
    xSemaphoreGive(mutex_);
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
