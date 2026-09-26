// SPDX-License-Identifier: Apache-2.0
#include "cajui_discovery.h"
#include "cajui_text.h"
#include <cinttypes>
#include <cstring>

namespace cajui {
namespace {
using Text = TextBuffer;
constexpr uint16_t TemperatureMetric = 1, HumidityMetric = 2;
constexpr uint32_t ExpiryIntervals = 3; // As the silence alert: three missed samples.
constexpr unsigned IdSuffixDigits = 4;
constexpr uint64_t IdSuffixMask = 0xffff;

struct Kind {
    const char* object; // Object ID suffix and unique ID part.
    const char* name;
    const char* deviceClass; // Null: none.
    const char* unit;        // Null: none.
    bool diagnostic;
};
const Kind* kind(Entity entity) {
    static const Kind kinds[] = {
        {"temperature", "Temperature", "temperature",
         "\xc2\xb0"
         "C",
         false},
        {"humidity", "Humidity", "humidity", "%", false},
        {"rssi", "Signal strength", "signal_strength", "dBm", true},
        {"snr", "Signal-to-noise ratio", nullptr, "dB", true},
        {"wifi_rssi", "Wi-Fi signal", "signal_strength", "dBm", true},
        {"queue", "Samples waiting", nullptr, nullptr, true},
        {"uptime", "Uptime", "duration", "s", true},
    };
    const size_t index = size_t(entity);
    return index < sizeof(kinds) / sizeof(kinds[0]) ? &kinds[index] : nullptr;
}
bool onReceiver(Entity entity) {
    return entity == Entity::WifiRssi || entity == Entity::QueueDepth || entity == Entity::Uptime;
}
bool radio(Entity entity) {
    return entity == Entity::Rssi || entity == Entity::Snr;
}
} // namespace

bool formatDiscoveryTopic(const char* source, const DiscoveryItem& item, char* output,
                          size_t capacity) {
    const Kind* k = kind(item.entity);
    if (!source || !validIdentity(source) || !k || !output || !capacity) return false;
    Text text(output, capacity);
    text.format("homeassistant/sensor/%s/%016" PRIx64 "_%s", source, item.device, k->object);
    if (!onReceiver(item.entity) && !radio(item.entity)) text.format("_%u", unsigned(item.sensor));
    text.format("/config");
    return text.ok();
}

bool formatDiscovery(const char* source, uint64_t receiver, const DiscoveryItem& item, char* output,
                     size_t capacity, size_t& size) {
    size = 0;
    const Kind* k = kind(item.entity);
    const bool own = onReceiver(item.entity);
    if (!source || !validIdentity(source) || !k || !output || !capacity ||
        (!own && item.intervalS == 0) || (own && item.device != receiver))
        return false;
    Text text(output, capacity);
    text.format("{\"name\":\"%s\",\"uniq_id\":\"cajui_%016" PRIx64 "_%s", k->name, item.device,
                k->object);
    if (!own && !radio(item.entity)) text.format("_%u", unsigned(item.sensor));
    text.format("\",\"qos\":1");
    if (own)
        text.format(",\"stat_t\":\"manage/v1/%s/%016" PRIx64 "/state\"", source, item.device);
    else
        text.format(",\"stat_t\":\"telemetry/v1/%s/%016" PRIx64 "/samples\",\"exp_aft\":%" PRIu32,
                    source, item.device, item.intervalS * ExpiryIntervals);
    if (k->deviceClass) text.format(",\"dev_cla\":\"%s\"", k->deviceClass);
    if (k->unit) text.format(",\"unit_of_meas\":\"%s\"", k->unit);
    text.format(",\"stat_cla\":\"measurement\"");
    if (k->diagnostic) text.format(",\"ent_cat\":\"diagnostic\"");
    // Values are selected by sensor and metric, never by array position, and anything
    // but an ok reading becomes unknown rather than zero.
    switch (item.entity) {
    case Entity::WifiRssi: text.format(",\"val_tpl\":\"{{ value_json.wifi.rssi_dbm }}\""); break;
    case Entity::QueueDepth: text.format(",\"val_tpl\":\"{{ value_json.queue.depth }}\""); break;
    case Entity::Uptime: text.format(",\"val_tpl\":\"{{ value_json.uptime_s }}\""); break;
    default: {
        char sensor[16]{};
        Text id(sensor, sizeof(sensor));
        if (radio(item.entity))
            id.format("radio");
        else
            id.format("sensor-%u", unsigned(item.sensor));
        text.format(",\"val_tpl\":\"{%% set r = value_json.readings | selectattr('sensor_id', "
                    "'equalto', '%s') | selectattr('metric', 'equalto', '%s') | list %%}{{ "
                    "r[0].value if r and r[0].status == 'ok' else 'None' }}\"",
                    sensor, k->object);
    }
    }
    text.format(",\"avty_t\":\"manage/v1/%s/%016" PRIx64 "/availability\"", source, receiver);
    const unsigned suffix = unsigned(item.device & IdSuffixMask);
    if (own)
        text.format(",\"dev\":{\"ids\":[\"cajui_%016" PRIx64 "\"],\"name\":\"Cajui receiver "
                    "%0*X\",\"mf\":\"Cajui\",\"mdl\":\"Receiver\"}}",
                    item.device, int(IdSuffixDigits), suffix);
    else
        text.format(
            ",\"dev\":{\"ids\":[\"cajui_%016" PRIx64 "\"],\"name\":\"Cajui transmitter "
            "%0*X\",\"mf\":\"Cajui\",\"mdl\":\"Transmitter\",\"via_device\":\"cajui_%016" PRIx64
            "\"}}",
            item.device, int(IdSuffixDigits), suffix, receiver);
    if (!text.ok()) return false;
    size = text.size();
    return true;
}

DiscoveryReporter::DiscoveryReporter(StatePublisher& publisher, uint64_t receiver,
                                     const char* source)
    : publisher_(publisher), receiver_(receiver) {
    setSource(source);
}
bool DiscoveryReporter::setSource(const char* source) {
    if (!source || !validIdentity(source)) {
        valid_ = false;
        return false;
    }
    std::memcpy(source_, source, std::strlen(source) + 1);
    valid_ = true;
    receiverPublished_ = 0;
    for (auto& n : nodes_) n.published = 0;
    return true;
}
void DiscoveryReporter::sampleForwarded(const QueuedSample& sample) {
    Node* slot = nullptr;
    for (auto& n : nodes_)
        if (n.node == sample.node) slot = &n;
    for (auto& n : nodes_)
        if (!slot && n.node == 0) slot = &n;
    if (!slot) return; // More nodes than bindings cannot happen; nothing to learn then.
    if (slot->node != sample.node) *slot = Node{};
    slot->node = sample.node;
    if (slot->intervalS != sample.data.nextSeconds) slot->published = 0;
    slot->intervalS = sample.data.nextSeconds;
    for (size_t i = 0; i < sample.data.count && i < MaxReadings; ++i) {
        const Reading& reading = sample.data.readings[i];
        const Entity entity = reading.metric == TemperatureMetric ? Entity::Temperature
                              : reading.metric == HumidityMetric  ? Entity::Humidity
                                                                  : Entity::Rssi;
        if (entity == Entity::Rssi) continue; // Only the registry's known metrics.
        // One sensor per transmitter in the current registry; the first one seen names it.
        if (!slot->sensor) slot->sensor = reading.sensor;
        if (reading.sensor == slot->sensor) slot->known |= uint8_t(1u << unsigned(entity));
    }
    if (sample.link.known)
        slot->known |= uint8_t(1u << unsigned(Entity::Rssi) | 1u << unsigned(Entity::Snr));
}
bool DiscoveryReporter::publish(const DiscoveryItem& item) {
    size_t size = 0;
    return formatDiscoveryTopic(source_, item, topic_, sizeof(topic_)) &&
           formatDiscovery(source_, receiver_, item, payload_, sizeof(payload_), size) &&
           publisher_.publishRetained(topic_, payload_, size);
}
bool DiscoveryReporter::poll() {
    if (!valid_ || !publisher_.ready()) return false;
    const uint32_t session = publisher_.session();
    if (session != session_) {
        session_ = session;
        receiverPublished_ = 0;
        for (auto& n : nodes_) n.published = 0;
    }
    for (unsigned i = 0; i < ReceiverEntities; ++i) {
        if (receiverPublished_ & (1u << i)) continue;
        DiscoveryItem item{};
        item.device = receiver_;
        item.entity = Entity(unsigned(Entity::WifiRssi) + i);
        // A refused publication is retried on a later pass.
        if (publish(item)) receiverPublished_ |= uint8_t(1u << i);
        return true;
    }
    for (auto& n : nodes_) {
        const uint8_t due = uint8_t(n.known & ~n.published);
        if (!n.node || !due) continue;
        for (unsigned i = 0; i < NodeEntities; ++i) {
            if (!(due & (1u << i))) continue;
            DiscoveryItem item{};
            item.device = n.node;
            item.sensor = n.sensor;
            item.entity = Entity(i);
            item.intervalS = n.intervalS;
            if (publish(item)) n.published |= uint8_t(1u << i);
            return true;
        }
    }
    return false;
}
} // namespace cajui
