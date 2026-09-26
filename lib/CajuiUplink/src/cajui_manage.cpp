// SPDX-License-Identifier: Apache-2.0
#include "cajui_manage.h"
#include "cajui_text.h"
#include <cinttypes>
#include <cstring>

namespace cajui {
namespace {
using Text = TextBuffer;
constexpr uint32_t VersionMajor = 10000, VersionMinor = 100;
constexpr unsigned TenthsPerDb = 10;

const char* bindingName(NodeBinding binding) {
    switch (binding) {
    case NodeBinding::Active: return "active";
    case NodeBinding::Pending: return "pending";
    case NodeBinding::Revoked: return "revoked";
    }
    return nullptr;
}
const char* leafName(ManageTopic topic) {
    switch (topic) {
    case ManageTopic::Availability: return "availability";
    case ManageTopic::State: return "state";
    case ManageTopic::Commands: return "commands";
    case ManageTopic::Results: return "results";
    }
    return nullptr;
}
// Values come from the firmware itself, never from a request, but a quote or control
// character would still break the document: such a value is refused.
bool plain(const char* value) {
    for (; *value; ++value)
        if (*value == '"' || *value == '\\' || uint8_t(*value) < 0x20) return false;
    return true;
}
// Callers check plainStrings first.
void addString(Text& text, const char* value) {
    if (value)
        text.format("\"%s\"", value);
    else
        text.format("null");
}
bool plainStrings(const ReceiverStatus& status) {
    for (const char* value : {status.model, status.slot, status.firmwareState, status.resetReason})
        if (value && !plain(value)) return false;
    return true;
}
void addTenths(Text& text, int16_t tenths) {
    const unsigned magnitude = unsigned(tenths < 0 ? -tenths : tenths);
    text.format("%s%u.%u", tenths < 0 ? "-" : "", magnitude / TenthsPerDb, magnitude % TenthsPerDb);
}
} // namespace

bool formatManageTopic(const char* source, uint64_t device, ManageTopic topic, char* output,
                       size_t capacity) {
    const char* leaf = leafName(topic);
    if (!source || !validIdentity(source) || !leaf || !output || !capacity) return false;
    Text text(output, capacity);
    text.format("manage/v1/%s/%016" PRIx64 "/%s", source, device, leaf);
    return text.ok();
}

bool formatReceiverState(const char* source, const ReceiverStatus& status, char* output,
                         size_t capacity, size_t& size) {
    size = 0;
    if (!source || !validIdentity(source) || !output || !capacity || !plainStrings(status) ||
        status.requestCount > MaxCandidates || (status.requestCount && !status.requests))
        return false;
    Text text(output, capacity);
    text.format("{\"version\":1,\"source_id\":\"%s\",\"device_id\":\"%016" PRIx64
                "\",\"role\":\"receiver\",\"model\":",
                source, status.device);
    addString(text, status.model);
    text.format(",\"firmware\":{\"version\":\"%" PRIu32 ".%" PRIu32 ".%" PRIu32 "\",\"slot\":",
                status.firmwareVersion / VersionMajor,
                status.firmwareVersion / VersionMinor % VersionMinor,
                status.firmwareVersion % VersionMinor);
    addString(text, status.slot);
    text.format(",\"state\":");
    addString(text, status.firmwareState);
    text.format("},\"radio\":{\"profile\":%u,\"power_dbm\":%d},\"uptime_s\":%" PRIu32
                ",\"reset_reason\":",
                unsigned(status.profile), int(status.powerDbm), status.uptimeS);
    addString(text, status.resetReason);
    if (status.wifiKnown)
        text.format(",\"wifi\":{\"rssi_dbm\":%d}", int(status.wifiRssiDbm));
    else
        text.format(",\"wifi\":{\"rssi_dbm\":null}");
    text.format(",\"queue\":{\"depth\":%u,\"capacity\":%u},\"forwarding\":{\"published\":%" PRIu32
                ",\"retries\":%" PRIu32 "},\"pairing\":{\"open\":%s,\"remaining_s\":%" PRIu32
                ",\"requests\":[",
                unsigned(status.queued), unsigned(status.queueCapacity), status.published,
                status.retries, status.pairingOpen ? "true" : "false",
                status.pairingOpen ? status.pairingRemainingS : 0);
    for (size_t i = 0; i < status.requestCount; ++i) {
        const Candidate& request = status.requests[i];
        text.format("%s{\"node_id\":\"%016" PRIx64 "\",\"rssi_dbm\":%d,\"conflict\":%s}",
                    i ? "," : "", request.node, int(request.rssi),
                    request.conflict ? "true" : "false");
    }
    text.format("]},\"capabilities\":[%s]}", status.commands ? "\"pairing\",\"revoke\"" : "");
    if (!text.ok()) return false;
    size = text.size();
    return true;
}

bool formatNodeState(const char* source, const NodeStatus& status, char* output, size_t capacity,
                     size_t& size) {
    size = 0;
    const char* binding = bindingName(status.binding);
    if (!source || !validIdentity(source) || !binding || !output || !capacity) return false;
    Text text(output, capacity);
    text.format("{\"version\":1,\"source_id\":\"%s\",\"device_id\":\"%016" PRIx64
                "\",\"role\":\"transmitter\",\"receiver_id\":\"%016" PRIx64
                "\",\"binding\":\"%s\",\"model\":null,\"firmware\":null,\"last_frame\":",
                source, status.node, status.receiver, binding);
    if (!status.frameKnown) {
        text.format("null");
    } else {
        text.format("{\"counter\":%" PRIu64 ",\"rssi_dbm\":", status.counter);
        if (status.link.known) {
            text.format("%d,\"snr_db\":", int(status.link.rssiDbm));
            addTenths(text, status.link.snrTenthsDb);
        } else {
            text.format("null,\"snr_db\":null");
        }
        text.format(",\"receiver_uptime_s\":%" PRIu32 "}", status.receiverUptimeS);
    }
    text.format(",\"parameters\":{\"interval_s\":null,\"power_dbm\":null},\"pending\":[]}");
    if (!text.ok()) return false;
    size = text.size();
    return true;
}

StateReporter::StateReporter(StatePublisher& publisher, StateSource& source, Clock& clock,
                             const char* name)
    : publisher_(publisher), source_(source), clock_(clock) {
    setSource(name);
}
bool StateReporter::setSource(const char* name) {
    if (!name || !validIdentity(name)) {
        valid_ = false;
        return false;
    }
    std::memcpy(name_, name, std::strlen(name) + 1);
    valid_ = true;
    markAll();
    return true;
}
// Strings come from constant tables, so comparing pointers compares values.
bool StateReporter::same(const Discrete& a, const Discrete& b) {
    if (a.firmwareVersion != b.firmwareVersion || a.slot != b.slot ||
        a.firmwareState != b.firmwareState || a.profile != b.profile || a.powerDbm != b.powerDbm ||
        a.pairingOpen != b.pairingOpen || a.requestCount != b.requestCount)
        return false;
    for (size_t i = 0; i < a.requestCount; ++i)
        if (a.requests[i] != b.requests[i] || a.conflicts[i] != b.conflicts[i]) return false;
    return true;
}
void StateReporter::markAll() {
    receiverDue_ = true;
    delayed_ = false;
    dirtyCount_ = source_.nodes(dirty_, BindingCapacity);
}
void StateReporter::nodeChanged(uint64_t node) {
    for (size_t i = 0; i < dirtyCount_; ++i)
        if (dirty_[i] == node) return;
    if (dirtyCount_ < BindingCapacity) dirty_[dirtyCount_++] = node;
}
void StateReporter::poll() {
    if (!valid_ || !publisher_.ready()) return;
    const uint32_t session = publisher_.session();
    if (session != session_) {
        session_ = session;
        markAll();
    }
    const uint32_t now = clock_.nowMs();
    if (delayed_ && int32_t(now - retryAt_) < 0) return;
    delayed_ = false;
    if (!receiverDue_ && checked_ && uint32_t(now - checkedAt_) < CheckMs &&
        uint32_t(now - receiverAt_) < CounterIntervalMs) {
        publishDirtyNode(now);
        return;
    }
    checked_ = true;
    checkedAt_ = now;
    ReceiverStatus status{};
    source_.receiverStatus(status);
    Discrete current{};
    current.firmwareVersion = status.firmwareVersion;
    current.slot = status.slot;
    current.firmwareState = status.firmwareState;
    current.profile = status.profile;
    current.powerDbm = status.powerDbm;
    current.pairingOpen = status.pairingOpen;
    current.requestCount =
        status.requestCount > MaxCandidates ? MaxCandidates : status.requestCount;
    for (size_t i = 0; i < current.requestCount; ++i) {
        current.requests[i] = status.requests[i].node;
        current.conflicts[i] = status.requests[i].conflict;
    }
    const bool changed = !same(current, discrete_);
    if (receiverDue_ || changed || uint32_t(now - receiverAt_) >= CounterIntervalMs) {
        if (!publishReceiver(status)) {
            delayed_ = true;
            retryAt_ = now + RetryMs;
            return;
        }
        discrete_ = current;
        receiverDue_ = false;
        receiverAt_ = now;
        return;
    }
    publishDirtyNode(now);
}
void StateReporter::publishDirtyNode(uint32_t now) {
    if (!dirtyCount_) return;
    if (!publishNode(dirty_[0])) {
        delayed_ = true;
        retryAt_ = now + RetryMs;
        return;
    }
    std::memmove(dirty_, dirty_ + 1, (dirtyCount_ - 1) * sizeof(dirty_[0]));
    --dirtyCount_;
}
bool StateReporter::publishReceiver(const ReceiverStatus& status) {
    size_t size = 0;
    return formatManageTopic(name_, status.device, ManageTopic::State, topic_, sizeof(topic_)) &&
           formatReceiverState(name_, status, payload_, sizeof(payload_), size) &&
           publisher_.publishRetained(topic_, payload_, size);
}
bool StateReporter::publishNode(uint64_t node) {
    NodeStatus status{};
    size_t size = 0;
    // A node that no longer exists (its slot was retired) has nothing to report.
    if (!source_.nodeStatus(node, status)) return true;
    return formatManageTopic(name_, node, ManageTopic::State, topic_, sizeof(topic_)) &&
           formatNodeState(name_, status, payload_, sizeof(payload_), size) &&
           publisher_.publishRetained(topic_, payload_, size);
}
} // namespace cajui
