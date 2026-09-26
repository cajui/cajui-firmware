// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cstring>
#include <deque>
#include <string>
#include "assertions.h"
#include "cajui_discovery.h"

namespace {
using namespace cajui;
constexpr uint64_t Receiver = 0x48ca433c5e10ULL, Node = 0x48ca433c776cULL;

DiscoveryItem item(uint64_t device, Entity entity, uint16_t sensor = 0, uint32_t interval = 0) {
    DiscoveryItem i{};
    i.device = device;
    i.entity = entity;
    i.sensor = sensor;
    i.intervalS = interval;
    return i;
}
std::string topic(const DiscoveryItem& i, const char* source = "receiver-1") {
    char out[TopicCapacity]{};
    if (!formatDiscoveryTopic(source, i, out, sizeof(out))) return "<rejected>";
    return out;
}
std::string payload(const DiscoveryItem& i, const char* source = "receiver-1",
                    uint64_t receiver = Receiver) {
    char out[DiscoveryCapacity]{};
    size_t size = 0;
    if (!formatDiscovery(source, receiver, i, out, sizeof(out), size)) return "<rejected>";
    TEST_ASSERT_EQUAL_size_t(std::strlen(out), size);
    return out;
}

void test_discovery_topics_sit_under_the_source() {
    TEST_ASSERT_EQUAL_STRING(
        "homeassistant/sensor/receiver-1/000048ca433c776c_temperature_1/config",
        topic(item(Node, Entity::Temperature, 1, 300)).c_str());
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/receiver-1/000048ca433c776c_snr/config",
                             topic(item(Node, Entity::Snr, 0, 300)).c_str());
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/receiver-1/000048ca433c5e10_uptime/config",
                             topic(item(Receiver, Entity::Uptime)).c_str());
    // The longest source and object still fit.
    const std::string longest(64, 's');
    TEST_ASSERT_NOT_EQUAL(
        0,
        std::strcmp("<rejected>",
                    topic(item(Node, Entity::Temperature, 65535, 300), longest.c_str()).c_str()));
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             topic(item(Node, Entity::Humidity), "bad source").c_str());
    // Valid for telemetry, illegal in Home Assistant's node level.
    TEST_ASSERT_EQUAL_STRING("<rejected>", topic(item(Node, Entity::Humidity), "site.rx1").c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", topic(item(Node, Entity::Humidity), "site:rx1").c_str());
    TEST_ASSERT_FALSE(validDiscoverySource(nullptr));
    TEST_ASSERT_TRUE(validDiscoverySource("rx_1-a"));
    TEST_ASSERT_EQUAL_STRING("<rejected>", topic(item(Node, Entity(99))).c_str());
    char small[8]{};
    TEST_ASSERT_FALSE(formatDiscoveryTopic("r", item(Node, Entity::Snr), small, sizeof(small)));
    TEST_ASSERT_FALSE(formatDiscoveryTopic("r", item(Node, Entity::Snr), nullptr, 8));
}
void test_transmitter_measurement_configuration() {
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Temperature\",\"uniq_id\":\"cajui_000048ca433c776c_temperature_1\","
        "\"qos\":1,\"stat_t\":\"telemetry/v1/receiver-1/000048ca433c776c/samples\","
        "\"exp_aft\":900,\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\xc2\xb0"
        "C\","
        "\"stat_cla\":\"measurement\",\"val_tpl\":\"{% set r = value_json.readings | "
        "selectattr('sensor_id', 'equalto', 'sensor-1') | selectattr('metric', 'equalto', "
        "'temperature') | list %}{{ r[0].value if r and r[0].status == 'ok' else 'None' }}\","
        "\"avty_t\":\"manage/v1/receiver-1/000048ca433c5e10/availability\",\"dev\":{\"ids\":["
        "\"cajui_000048ca433c776c\"],\"name\":\"Cajui transmitter 776C\",\"mf\":\"Cajui\","
        "\"mdl\":\"Transmitter\",\"via_device\":\"cajui_000048ca433c5e10\"}}",
        payload(item(Node, Entity::Temperature, 1, 300)).c_str());
    const std::string snr = payload(item(Node, Entity::Snr, 0, 60));
    TEST_ASSERT_NOT_NULL(
        std::strstr(snr.c_str(), "'equalto', 'radio') | selectattr('metric', 'equalto', 'snr')"));
    TEST_ASSERT_NOT_NULL(std::strstr(snr.c_str(), "\"exp_aft\":180,"));
    TEST_ASSERT_NULL(std::strstr(snr.c_str(), "dev_cla"));
    // A huge interval is clamped as telemetry does, never wrapped.
    TEST_ASSERT_NOT_NULL(std::strstr(payload(item(Node, Entity::Snr, 0, 1431655766)).c_str(),
                                     "\"exp_aft\":1814400,"));
    TEST_ASSERT_NOT_NULL(std::strstr(snr.c_str(), "\"ent_cat\":\"diagnostic\""));
    TEST_ASSERT_NOT_NULL(std::strstr(payload(item(Node, Entity::Humidity, 2, 300)).c_str(),
                                     "\"uniq_id\":\"cajui_000048ca433c776c_humidity_2\""));
}
void test_receiver_diagnostic_configuration() {
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Wi-Fi signal\",\"uniq_id\":\"cajui_000048ca433c5e10_wifi_rssi\",\"qos\":1,"
        "\"stat_t\":\"manage/v1/receiver-1/000048ca433c5e10/"
        "state\",\"dev_cla\":\"signal_strength\","
        "\"unit_of_meas\":\"dBm\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\","
        "\"val_tpl\":\"{{ value_json.wifi.rssi_dbm }}\",\"avty_t\":\"manage/v1/receiver-1/"
        "000048ca433c5e10/availability\",\"dev\":{\"ids\":[\"cajui_000048ca433c5e10\"],"
        "\"name\":\"Cajui receiver 5E10\",\"mf\":\"Cajui\",\"mdl\":\"Receiver\"}}",
        payload(item(Receiver, Entity::WifiRssi)).c_str());
    TEST_ASSERT_NOT_NULL(std::strstr(payload(item(Receiver, Entity::QueueDepth)).c_str(),
                                     "{{ value_json.queue.depth }}"));
    TEST_ASSERT_NOT_NULL(std::strstr(payload(item(Receiver, Entity::Uptime)).c_str(),
                                     "\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\""));
}
void test_invalid_configurations_are_rejected() {
    TEST_ASSERT_EQUAL_STRING("<rejected>", payload(item(Node, Entity::Temperature, 1, 0)).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", payload(item(Node, Entity::Uptime)).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", payload(item(Node, Entity(99), 1, 300)).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             payload(item(Receiver, Entity::Uptime), "bad source").c_str());
    char small[64]{};
    size_t size = 1;
    TEST_ASSERT_FALSE(
        formatDiscovery("r", Receiver, item(Receiver, Entity::Uptime), small, sizeof(small), size));
    TEST_ASSERT_EQUAL_size_t(0, size);
    TEST_ASSERT_FALSE(
        formatDiscovery("r", Receiver, item(Receiver, Entity::Uptime), nullptr, 8, size));
}

class Publisher final : public StatePublisher {
public:
    bool online = false, refuse = false;
    uint32_t connections = 0;
    std::deque<std::string> topics;
    bool ready() override { return online; }
    uint32_t session() override { return connections; }
    bool publishRetained(const char* t, const char* payload, size_t size) override {
        TEST_ASSERT_EQUAL_size_t(std::strlen(payload), size);
        if (refuse) return false;
        topics.emplace_back(t);
        return true;
    }
    void connect() {
        online = true;
        ++connections;
    }
};
QueuedSample sample(uint64_t node, uint32_t interval, bool link) {
    QueuedSample s{};
    s.node = node;
    s.data.nextSeconds = interval;
    s.data.count = 3;
    s.data.readings[0].sensor = 1;
    s.data.readings[0].metric = 1;
    s.data.readings[1].sensor = 1;
    s.data.readings[1].metric = 2;
    s.data.readings[2].sensor = 1;
    s.data.readings[2].metric = 9; // Not in the registry: no entity.
    s.link.known = link;
    return s;
}
int drain(DiscoveryReporter& reporter) {
    int published = 0;
    for (int i = 0; i < 64 && reporter.poll(); ++i) ++published;
    return published;
}

void test_reporter_publishes_the_receiver_then_learned_transmitters() {
    Publisher publisher;
    DiscoveryReporter reporter(publisher, Receiver, "receiver-1");
    TEST_ASSERT_FALSE(reporter.poll()); // Offline.
    publisher.connect();
    TEST_ASSERT_EQUAL_INT(3, drain(reporter));
    TEST_ASSERT_NOT_NULL(
        std::strstr(publisher.topics.front().c_str(), "000048ca433c5e10_wifi_rssi"));
    publisher.topics.clear();
    reporter.sampleForwarded(sample(Node, 300, false));
    TEST_ASSERT_EQUAL_INT(2, drain(reporter)); // Temperature and humidity.
    reporter.sampleForwarded(sample(Node, 300, true));
    TEST_ASSERT_EQUAL_INT(2, drain(reporter)); // Now the radio link too.
    reporter.sampleForwarded(sample(Node, 300, true));
    TEST_ASSERT_EQUAL_INT(0, drain(reporter)); // Nothing new.
    reporter.sampleForwarded(sample(Node, 60, true));
    TEST_ASSERT_EQUAL_INT(4, drain(reporter)); // A new interval changes expire_after.
    publisher.connect();
    TEST_ASSERT_EQUAL_INT(7, drain(reporter)); // Everything again after a reconnection.
}
void test_reporter_retries_refusals_and_follows_the_source() {
    Publisher publisher;
    DiscoveryReporter reporter(publisher, Receiver, "receiver-1");
    publisher.connect();
    publisher.refuse = true;
    TEST_ASSERT_TRUE(reporter.poll()); // It waited for the client even though refused.
    publisher.refuse = false;
    // Then it steps aside for a while so the state reporter keeps its turns.
    for (int i = 0; i < 50; ++i) TEST_ASSERT_FALSE(reporter.poll());
    TEST_ASSERT_EQUAL_INT(3, drain(reporter));
    reporter.pause();
    TEST_ASSERT_FALSE(reporter.poll());
    TEST_ASSERT_FALSE(reporter.setSource("bad source"));
    TEST_ASSERT_FALSE(reporter.setSource("site.rx1"));
    TEST_ASSERT_FALSE(reporter.poll());
    TEST_ASSERT_TRUE(reporter.setSource("receiver-2"));
    publisher.topics.clear();
    TEST_ASSERT_EQUAL_INT(3, drain(reporter));
    TEST_ASSERT_NOT_NULL(std::strstr(publisher.topics.front().c_str(), "/receiver-2/"));
    // Beyond the slots, the transmitter seen longest ago gives way to a new one.
    for (uint64_t node = 1; node <= BindingCapacity; ++node)
        reporter.sampleForwarded(sample(node, 300, false));
    TEST_ASSERT_EQUAL_INT(int(BindingCapacity) * 2, drain(reporter));
    reporter.sampleForwarded(sample(1, 300, false)); // Node 1 is recent again.
    reporter.sampleForwarded(sample(BindingCapacity + 1, 300, false));
    publisher.topics.clear();
    TEST_ASSERT_EQUAL_INT(2, drain(reporter));
    TEST_ASSERT_NOT_NULL(std::strstr(publisher.topics.front().c_str(), "0000000000000011_"));
    reporter.sampleForwarded(sample(1, 300, false)); // Still known: nothing new.
    TEST_ASSERT_EQUAL_INT(0, drain(reporter));
}
} // namespace

void runDiscoveryTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_discovery_topics_sit_under_the_source);
    RUN_TEST(test_transmitter_measurement_configuration);
    RUN_TEST(test_receiver_diagnostic_configuration);
    RUN_TEST(test_invalid_configurations_are_rejected);
    RUN_TEST(test_reporter_publishes_the_receiver_then_learned_transmitters);
    RUN_TEST(test_reporter_retries_refusals_and_follows_the_source);
}
