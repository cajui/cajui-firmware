// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "assertions.h"
#include "cajui_manage.h"

namespace {
using namespace cajui;

std::string topic(const char* source, uint64_t device, ManageTopic kind) {
    char out[TopicCapacity]{};
    if (!formatManageTopic(source, device, kind, out, sizeof(out))) return "<rejected>";
    return out;
}
Candidate request(uint64_t node, int16_t rssi, bool conflict) {
    Candidate c{};
    c.node = node;
    c.rssi = rssi;
    c.conflict = conflict;
    return c;
}
ReceiverStatus sampleReceiver() {
    ReceiverStatus s{};
    s.device = 0x5e10;
    s.model = "heltec-wifi-lora-32-v3";
    s.firmwareVersion = 20003;
    s.slot = "ota_1";
    s.firmwareState = "valid";
    s.profile = 1;
    s.powerDbm = -9;
    s.uptimeS = 3600;
    s.resetReason = "power_on";
    s.wifiKnown = true;
    s.wifiRssiDbm = -61;
    s.queued = 3;
    s.queueCapacity = QueueCapacity;
    s.published = 42;
    s.retries = 1;
    return s;
}
std::string receiverJson(const ReceiverStatus& status, size_t capacity = StateCapacity) {
    std::vector<char> out(capacity ? capacity : 1);
    size_t size = 0;
    if (!formatReceiverState("receiver-1", status, out.data(), capacity, size)) return "<rejected>";
    TEST_ASSERT_EQUAL_size_t(std::strlen(out.data()), size);
    return out.data();
}
NodeStatus sampleNode() {
    NodeStatus s{};
    s.node = 0xa1;
    s.receiver = 0x5e10;
    s.binding = NodeBinding::Active;
    s.frameKnown = true;
    s.counter = 124;
    s.link.known = true;
    s.link.rssiDbm = -82;
    s.link.snrTenthsDb = 95;
    s.receiverUptimeS = 3590;
    return s;
}
std::string nodeJson(const NodeStatus& status, const char* source = "receiver-1") {
    char out[StateCapacity]{};
    size_t size = 0;
    if (!formatNodeState(source, status, out, sizeof(out), size)) return "<rejected>";
    TEST_ASSERT_EQUAL_size_t(std::strlen(out), size);
    return out;
}

void test_management_topics_follow_the_contract() {
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/0000000000005e10/availability",
                             topic("receiver-1", 0x5e10, ManageTopic::Availability).c_str());
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/00000000000000a1/state",
                             topic("receiver-1", 0xa1, ManageTopic::State).c_str());
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/ffffffffffffffff/commands",
                             topic("receiver-1", UINT64_MAX, ManageTopic::Commands).c_str());
    TEST_ASSERT_EQUAL_STRING("manage/v1/r/0000000000000001/results",
                             topic("r", 1, ManageTopic::Results).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", topic("bad source", 1, ManageTopic::State).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", topic(nullptr, 1, ManageTopic::State).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", topic("r", 1, ManageTopic(9)).c_str());
    char small[8]{};
    TEST_ASSERT_FALSE(formatManageTopic("r", 1, ManageTopic::State, small, sizeof(small)));
    TEST_ASSERT_FALSE(formatManageTopic("r", 1, ManageTopic::State, nullptr, 8));
    TEST_ASSERT_FALSE(formatManageTopic("r", 1, ManageTopic::State, small, 0));
}
void test_receiver_state_matches_the_contract() {
    auto status = sampleReceiver();
    const Candidate requests[] = {request(0xa2, -70, false), request(0xa3, -95, true)};
    status.pairingOpen = true;
    status.pairingRemainingS = 87;
    status.requests = requests;
    status.requestCount = 2;
    TEST_ASSERT_EQUAL_STRING(
        "{\"version\":1,\"source_id\":\"receiver-1\",\"device_id\":\"0000000000005e10\","
        "\"role\":\"receiver\",\"model\":\"heltec-wifi-lora-32-v3\",\"firmware\":{\"version\":"
        "\"2.0.3\",\"slot\":\"ota_1\",\"state\":\"valid\"},\"radio\":{\"profile\":1,"
        "\"power_dbm\":-9},\"uptime_s\":3600,\"reset_reason\":\"power_on\",\"wifi\":{"
        "\"rssi_dbm\":-61},\"queue\":{\"depth\":3,\"capacity\":128},\"forwarding\":{"
        "\"published\":42,\"retries\":1},\"pairing\":{\"open\":true,\"remaining_s\":87,"
        "\"requests\":[{\"node_id\":\"00000000000000a2\",\"rssi_dbm\":-70,\"conflict\":false},"
        "{\"node_id\":\"00000000000000a3\",\"rssi_dbm\":-95,\"conflict\":true}]},"
        "\"capabilities\":[]}",
        receiverJson(status).c_str());
}
void test_unknown_receiver_values_are_null_never_zero() {
    ReceiverStatus status{};
    status.pairingRemainingS = 50; // A closed window reports no remaining time.
    TEST_ASSERT_EQUAL_STRING(
        "{\"version\":1,\"source_id\":\"receiver-1\",\"device_id\":\"0000000000000000\","
        "\"role\":\"receiver\",\"model\":null,\"firmware\":{\"version\":\"0.0.0\",\"slot\":null,"
        "\"state\":null},\"radio\":{\"profile\":0,\"power_dbm\":0},\"uptime_s\":0,"
        "\"reset_reason\":null,\"wifi\":{\"rssi_dbm\":null},\"queue\":{\"depth\":0,"
        "\"capacity\":0},\"forwarding\":{\"published\":0,\"retries\":0},\"pairing\":{"
        "\"open\":false,\"remaining_s\":0,\"requests\":[]},\"capabilities\":[]}",
        receiverJson(status).c_str());
}
void test_invalid_receiver_states_are_rejected() {
    const Candidate requests[MaxCandidates + 1]{};
    const char* unsafe[] = {"quote\"", "back\\slash", "line\nbreak"};
    for (const char* value : unsafe) {
        auto status = sampleReceiver();
        status.model = value;
        TEST_ASSERT_EQUAL_STRING("<rejected>", receiverJson(status).c_str());
        status = sampleReceiver();
        status.resetReason = value;
        TEST_ASSERT_EQUAL_STRING("<rejected>", receiverJson(status).c_str());
    }
    auto status = sampleReceiver();
    status.requests = requests;
    status.requestCount = MaxCandidates + 1;
    TEST_ASSERT_EQUAL_STRING("<rejected>", receiverJson(status).c_str());
    status.requests = nullptr;
    status.requestCount = 1;
    TEST_ASSERT_EQUAL_STRING("<rejected>", receiverJson(status).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", receiverJson(sampleReceiver(), 64).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", receiverJson(sampleReceiver(), 0).c_str());
    char out[StateCapacity]{};
    size_t size = 1;
    TEST_ASSERT_FALSE(formatReceiverState("bad source", sampleReceiver(), out, sizeof(out), size));
    TEST_ASSERT_EQUAL_size_t(0, size);
    TEST_ASSERT_FALSE(formatReceiverState(nullptr, sampleReceiver(), out, sizeof(out), size));
    TEST_ASSERT_FALSE(formatReceiverState("r", sampleReceiver(), nullptr, sizeof(out), size));
}
void test_node_state_matches_the_contract() {
    TEST_ASSERT_EQUAL_STRING(
        "{\"version\":1,\"source_id\":\"receiver-1\",\"device_id\":\"00000000000000a1\","
        "\"role\":\"transmitter\",\"receiver_id\":\"0000000000005e10\",\"binding\":\"active\","
        "\"model\":null,\"firmware\":null,\"last_frame\":{\"counter\":124,\"rssi_dbm\":-82,"
        "\"snr_db\":9.5,\"receiver_uptime_s\":3590},\"parameters\":{\"interval_s\":null,"
        "\"power_dbm\":null},\"pending\":[]}",
        nodeJson(sampleNode()).c_str());
    auto status = sampleNode();
    status.link.snrTenthsDb = -4;
    TEST_ASSERT_NOT_NULL(std::strstr(nodeJson(status).c_str(), "\"snr_db\":-0.4,"));
    status.link.known = false;
    TEST_ASSERT_NOT_NULL(
        std::strstr(nodeJson(status).c_str(), "\"rssi_dbm\":null,\"snr_db\":null,"));
    status.frameKnown = false;
    status.binding = NodeBinding::Pending;
    const std::string pending = nodeJson(status);
    TEST_ASSERT_NOT_NULL(std::strstr(pending.c_str(), "\"binding\":\"pending\""));
    TEST_ASSERT_NOT_NULL(std::strstr(pending.c_str(), "\"last_frame\":null,"));
    status.binding = NodeBinding::Revoked;
    TEST_ASSERT_NOT_NULL(std::strstr(nodeJson(status).c_str(), "\"binding\":\"revoked\""));
}
void test_invalid_node_states_are_rejected() {
    auto status = sampleNode();
    status.binding = NodeBinding(9);
    TEST_ASSERT_EQUAL_STRING("<rejected>", nodeJson(status).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", nodeJson(sampleNode(), "bad source").c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", nodeJson(sampleNode(), nullptr).c_str());
    char small[64]{};
    size_t size = 1;
    TEST_ASSERT_FALSE(formatNodeState("r", sampleNode(), small, sizeof(small), size));
    TEST_ASSERT_EQUAL_size_t(0, size);
    TEST_ASSERT_FALSE(formatNodeState("r", sampleNode(), nullptr, sizeof(small), size));
    TEST_ASSERT_FALSE(formatNodeState("r", sampleNode(), small, 0, size));
}

// --- Reporter --------------------------------------------------------------------------
class ManualClock final : public Clock {
public:
    uint32_t time = 5000;
    uint32_t nowMs() const override { return time; }
};
class RecordingPublisher final : public StatePublisher {
public:
    bool online = false, refuse = false;
    uint32_t connections = 0;
    std::deque<std::string> topics, payloads;
    bool ready() override { return online; }
    uint32_t session() override { return connections; }
    bool publishRetained(const char* topic, const char* payload, size_t size) override {
        TEST_ASSERT_EQUAL_size_t(std::strlen(payload), size);
        if (refuse) return false;
        topics.emplace_back(topic);
        payloads.emplace_back(payload);
        return true;
    }
    void clear() {
        topics.clear();
        payloads.clear();
    }
    void connect() {
        online = true;
        ++connections;
    }
    std::string take() {
        if (topics.empty()) return "<none>";
        std::string topic = topics.front();
        topics.pop_front();
        payloads.pop_front();
        return topic;
    }
};
class FakeSource final : public StateSource {
public:
    ReceiverStatus receiver = sampleReceiver();
    std::vector<uint64_t> known = {0xa1, 0xa2};
    uint64_t missing = 0;
    void receiverStatus(ReceiverStatus& out) override { out = receiver; }
    size_t nodes(uint64_t* output, size_t capacity) override {
        size_t count = 0;
        for (uint64_t node : known)
            if (count < capacity) output[count++] = node;
        return count;
    }
    bool nodeStatus(uint64_t node, NodeStatus& out) override {
        if (node == missing) return false;
        out = sampleNode();
        out.node = node;
        return true;
    }
};
struct ReportRig {
    ManualClock clock;
    RecordingPublisher publisher;
    FakeSource source;
    StateReporter reporter{publisher, source, clock, "receiver-1"};
    void polls(int count) {
        for (int i = 0; i < count; ++i) reporter.poll();
    }
};
const char* ReceiverTopic = "manage/v1/receiver-1/0000000000005e10/state";

void test_reporter_publishes_everything_after_each_connection() {
    ReportRig rig;
    rig.polls(3);
    TEST_ASSERT_TRUE(rig.publisher.topics.empty()); // Offline: nothing queued for later.
    rig.publisher.connect();
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_size_t(1, rig.publisher.topics.size()); // One publication per poll.
    rig.polls(5);
    TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/00000000000000a1/state",
                             rig.publisher.take().c_str());
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/00000000000000a2/state",
                             rig.publisher.take().c_str());
    TEST_ASSERT_EQUAL_STRING("<none>", rig.publisher.take().c_str());
    rig.publisher.online = false;
    rig.polls(2);
    rig.publisher.connect(); // Reconnection: the broker may have lost nothing, but resend.
    rig.polls(5);
    TEST_ASSERT_EQUAL_size_t(3, rig.publisher.topics.size());
}
void test_reporter_republishes_counters_once_a_minute_and_changes_at_once() {
    ReportRig rig;
    rig.publisher.connect();
    rig.polls(5);
    rig.publisher.clear();
    rig.source.receiver.uptimeS += 30;
    rig.source.receiver.queued = 9;
    rig.source.receiver.pairingRemainingS = 10; // Counter while closed: no change.
    rig.clock.time += StateReporter::CounterIntervalMs - 1;
    rig.polls(3);
    TEST_ASSERT_TRUE(rig.publisher.topics.empty());
    rig.clock.time += 1;
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_size_t(1, rig.publisher.payloads.size());
    TEST_ASSERT_NOT_NULL(std::strstr(rig.publisher.payloads.front().c_str(), "\"depth\":9"));
    TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());

    const Candidate requests[] = {request(0xb1, -60, false)};
    struct Change {
        void (*apply)(ReceiverStatus&, const Candidate*);
    };
    const Change changes[] = {
        {[](ReceiverStatus& s, const Candidate*) { s.pairingOpen = true; }},
        {[](ReceiverStatus& s, const Candidate* r) {
            s.requests = r;
            s.requestCount = 1;
        }},
        {[](ReceiverStatus& s, const Candidate*) { s.firmwareState = "pending"; }},
        {[](ReceiverStatus& s, const Candidate*) { s.slot = "ota_0"; }},
        {[](ReceiverStatus& s, const Candidate*) { s.firmwareVersion = 20004; }},
        {[](ReceiverStatus& s, const Candidate*) { s.profile = 2; }},
        {[](ReceiverStatus& s, const Candidate*) { s.powerDbm = 3; }},
    };
    size_t index = 0;
    for (const auto& change : changes) {
        SCENARIO(index++);
        change.apply(rig.source.receiver, requests);
        rig.clock.time += 10;
        rig.reporter.poll();
        TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());
        rig.reporter.poll();
        TEST_ASSERT_EQUAL_STRING("<none>", rig.publisher.take().c_str());
    }
    rig.source.receiver.pairingRemainingS = 3; // Counter: waits for the minute.
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_STRING("<none>", rig.publisher.take().c_str());
    Candidate conflicted = requests[0];
    conflicted.conflict = true;
    rig.source.receiver.requests = &conflicted;
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());
    const Candidate other[] = {request(0xb2, -60, false)};
    rig.source.receiver.requests = other;
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());
}
void test_reporter_publishes_changed_nodes_once_and_bounds_its_list() {
    ReportRig rig;
    rig.publisher.connect();
    rig.polls(5);
    rig.publisher.clear();
    rig.reporter.nodeChanged(0xa2);
    rig.reporter.nodeChanged(0xa2);
    rig.polls(3);
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/00000000000000a2/state",
                             rig.publisher.take().c_str());
    TEST_ASSERT_EQUAL_STRING("<none>", rig.publisher.take().c_str());
    for (uint64_t node = 1; node <= BindingCapacity + 3; ++node) rig.reporter.nodeChanged(node);
    rig.polls(int(BindingCapacity) + 5);
    TEST_ASSERT_EQUAL_size_t(BindingCapacity, rig.publisher.topics.size());
    rig.publisher.clear();
    rig.source.missing = 0x77; // Retired before its turn: skipped, not retried.
    rig.reporter.nodeChanged(0x77);
    rig.reporter.nodeChanged(0x78);
    rig.polls(3);
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/0000000000000078/state",
                             rig.publisher.take().c_str());
    TEST_ASSERT_EQUAL_STRING("<none>", rig.publisher.take().c_str());
}
void test_reporter_retries_a_refused_publication_later() {
    ReportRig rig;
    rig.publisher.connect();
    rig.publisher.refuse = true;
    rig.reporter.poll();
    rig.publisher.refuse = false;
    rig.clock.time += StateReporter::RetryMs - 1;
    rig.polls(3);
    TEST_ASSERT_TRUE(rig.publisher.topics.empty());
    rig.clock.time += 1;
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());
    rig.publisher.refuse = true;
    rig.reporter.poll(); // A node publication refused.
    rig.publisher.refuse = false;
    rig.reporter.poll();
    TEST_ASSERT_TRUE(rig.publisher.topics.empty());
    rig.clock.time += StateReporter::RetryMs;
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-1/00000000000000a1/state",
                             rig.publisher.take().c_str());
}
void test_reporter_switches_source_and_stops_without_a_valid_one() {
    ReportRig rig;
    rig.publisher.connect();
    rig.polls(5);
    rig.publisher.clear();
    TEST_ASSERT_FALSE(rig.reporter.setSource("bad source"));
    TEST_ASSERT_FALSE(rig.reporter.setSource(nullptr));
    rig.clock.time += StateReporter::CounterIntervalMs;
    rig.polls(3);
    TEST_ASSERT_TRUE(rig.publisher.topics.empty());
    TEST_ASSERT_TRUE(rig.reporter.setSource("receiver-2"));
    rig.polls(5);
    TEST_ASSERT_EQUAL_size_t(3, rig.publisher.topics.size());
    TEST_ASSERT_EQUAL_STRING("manage/v1/receiver-2/0000000000005e10/state",
                             rig.publisher.take().c_str());

    ManualClock clock;
    RecordingPublisher publisher;
    FakeSource source;
    StateReporter invalid(publisher, source, clock, "bad source");
    publisher.connect();
    invalid.poll();
    TEST_ASSERT_TRUE(publisher.topics.empty());
}
void test_reporter_skips_a_receiver_state_it_cannot_format() {
    ReportRig rig;
    rig.source.receiver.model = "bad\"model";
    rig.publisher.connect();
    rig.reporter.poll();
    TEST_ASSERT_TRUE(rig.publisher.topics.empty()); // Treated like a refused publication.
    rig.source.receiver.model = "heltec-wifi-lora-32-v3";
    rig.clock.time += StateReporter::RetryMs;
    rig.reporter.poll();
    TEST_ASSERT_EQUAL_STRING(ReceiverTopic, rig.publisher.take().c_str());
}
} // namespace

void runManageTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_management_topics_follow_the_contract);
    RUN_TEST(test_receiver_state_matches_the_contract);
    RUN_TEST(test_unknown_receiver_values_are_null_never_zero);
    RUN_TEST(test_invalid_receiver_states_are_rejected);
    RUN_TEST(test_node_state_matches_the_contract);
    RUN_TEST(test_invalid_node_states_are_rejected);
    RUN_TEST(test_reporter_publishes_everything_after_each_connection);
    RUN_TEST(test_reporter_republishes_counters_once_a_minute_and_changes_at_once);
    RUN_TEST(test_reporter_publishes_changed_nodes_once_and_bounds_its_list);
    RUN_TEST(test_reporter_retries_a_refused_publication_later);
    RUN_TEST(test_reporter_switches_source_and_stops_without_a_valid_one);
    RUN_TEST(test_reporter_skips_a_receiver_state_it_cannot_format);
}
