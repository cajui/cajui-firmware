#include <unity.h>
#include <cstring>
#include <deque>
#include <string>
#include "assertions.h"
#include "cajui_provisioning.h"
#include "cajui_uplink.h"
#include "storage_support.h"

namespace {
using namespace cajui;
using fixtures::MemoryBlob;

UplinkConfig validConfig() {
    UplinkConfig c{};
    std::strcpy(c.ssid, "Bench network");
    std::strcpy(c.wifiPassword, "correct horse");
    std::strcpy(c.host, "192.168.1.20");
    c.port = 1883;
    std::strcpy(c.username, "receiver-1");
    std::strcpy(c.password, "broker secret");
    return c;
}
QueuedSample queued(const Data& data) {
    QueuedSample s{};
    s.node = 2;
    s.generation = 10;
    s.counter = 7;
    s.data = data;
    return s;
}
Reading reading(uint16_t sensor, uint16_t metric, uint8_t unit, Status status, int32_t milli) {
    Reading r{};
    r.sensor = sensor;
    r.metric = metric;
    r.unit = unit;
    r.status = status;
    r.milliValue = milli;
    return r;
}
std::string format(const QueuedSample& sample, const char* source = "receiver-1") {
    char out[PayloadCapacity]{};
    size_t size = 0;
    if (!formatSample(source, sample, out, sizeof(out), size)) return "<rejected>";
    TEST_ASSERT_EQUAL_size_t(std::strlen(out), size);
    return out;
}

void test_uplink_settings_round_trip_and_fail_closed() {
    MemoryBlob blob;
    UplinkConfig loaded{};
    EXPECT_RESULT(ReadResult::Missing, loadUplink(blob, loaded));
    const auto config = validConfig();
    TEST_ASSERT_TRUE(saveUplink(blob, config));
    EXPECT_RESULT(ReadResult::Ok, loadUplink(blob, loaded));
    TEST_ASSERT_EQUAL_STRING(config.ssid, loaded.ssid);
    TEST_ASSERT_EQUAL_STRING(config.wifiPassword, loaded.wifiPassword);
    TEST_ASSERT_EQUAL_STRING(config.host, loaded.host);
    TEST_ASSERT_EQUAL_UINT16(1883, loaded.port);
    TEST_ASSERT_EQUAL_STRING(config.username, loaded.username);
    TEST_ASSERT_EQUAL_STRING(config.password, loaded.password);
    const auto good = blob.bytes;
    const size_t size = blob.size;
    // Flipped byte, wrong magic, future version, truncation and a trailing byte.
    const size_t corruptAt[] = {0, 4, 10, size - 1};
    for (size_t i = 0; i < 4; ++i) {
        SCENARIO(i);
        blob.bytes = good;
        blob.bytes[corruptAt[i]] ^= 1;
        EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
        TEST_ASSERT_EQUAL_STRING("", loaded.password);
    }
    blob.bytes = good;
    blob.size = size - 1;
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    blob.size = MinUplinkSize - 1;
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    blob.size = size + 1;
    blob.bytes[size] = 0;
    fixtures::repairChecksum(blob);
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    // A valid checksum does not make invalid content usable: port zero and a NUL in a string.
    blob.bytes = good;
    blob.size = size;
    blob.bytes[size - 6] = 0;
    blob.bytes[size - 5] = 0;
    fixtures::repairChecksum(blob);
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    blob.bytes = good;
    blob.bytes[7] = 0;
    fixtures::repairChecksum(blob);
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    blob.bytes = good;
    blob.bytes[5] = 200; // SSID length beyond its capacity.
    fixtures::repairChecksum(blob);
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    blob.bytes = good;
    blob.failRead = true;
    EXPECT_RESULT(ReadResult::Error, loadUplink(blob, loaded));
    blob.failRead = false;
    blob.failBefore = true;
    TEST_ASSERT_FALSE(saveUplink(blob, config));
}
void test_uplink_settings_are_validated_before_saving() {
    MemoryBlob blob;
    const auto base = validConfig();
    auto c = base;
    c.ssid[0] = 0;
    TEST_ASSERT_FALSE(validUplink(c));
    c = base;
    std::strcpy(c.wifiPassword, "short");
    TEST_ASSERT_FALSE(validUplink(c));
    c = base;
    std::strcpy(c.host, "bad host");
    TEST_ASSERT_FALSE(validUplink(c));
    c = base;
    c.host[0] = 0;
    TEST_ASSERT_FALSE(validUplink(c));
    c = base;
    std::memset(c.host, 'a', sizeof(c.host)); // No terminator within capacity.
    TEST_ASSERT_FALSE(validUplink(c));
    c = base;
    c.port = 0;
    TEST_ASSERT_FALSE(validUplink(c));
    c = base;
    c.password[0] = 0;
    TEST_ASSERT_FALSE(validUplink(c));
    const char* identities[] = {"", "-leading", "has space", "slash/topic", "plus+", "hash#"};
    for (auto identity : identities) {
        UNITY_SET_DETAIL(identity);
        c = base;
        std::strcpy(c.username, identity);
        TEST_ASSERT_FALSE(validUplink(c));
        TEST_ASSERT_FALSE(saveUplink(blob, c));
    }
    TEST_ASSERT_EQUAL_size_t(0, blob.writes);
    TEST_ASSERT_TRUE(validIdentity("a.b_c:d-9"));
    c = base;
    wipe(c);
    TEST_ASSERT_EQUAL_STRING("", c.wifiPassword);
    TEST_ASSERT_EQUAL_UINT16(0, c.port);
}
void test_sample_matches_the_central_mqtt_contract() {
    Data data{};
    data.nextSeconds = 300;
    data.count = 2;
    data.readings[0] = reading(1, 1, 1, Status::Ok, 26700);
    data.readings[1] = reading(1, 2, 2, Status::Ok, 61234);
    TEST_ASSERT_EQUAL_STRING(
        "{\"version\":1,\"source_id\":\"receiver-1\",\"device_id\":\"0000000000000002\","
        "\"sample_id\":\"000000000000000a.7\",\"expected_interval_seconds\":300,\"readings\":["
        "{\"sensor_id\":\"sensor-1\",\"metric\":\"temperature\",\"value\":26.700,"
        "\"unit\":\"degC\",\"status\":\"ok\"},"
        "{\"sensor_id\":\"sensor-1\",\"metric\":\"humidity\",\"value\":61.234,"
        "\"unit\":\"%\",\"status\":\"ok\"}]}",
        format(queued(data)).c_str());
    char topic[TopicCapacity]{};
    TEST_ASSERT_TRUE(formatTopic("receiver-1", 2, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("telemetry/v1/receiver-1/0000000000000002/samples", topic);
    TEST_ASSERT_FALSE(formatTopic("bad/source", 2, topic, sizeof(topic)));
    TEST_ASSERT_FALSE(formatTopic(nullptr, 2, topic, sizeof(topic)));
    TEST_ASSERT_FALSE(formatTopic("receiver-1", 2, nullptr, sizeof(topic)));
    TEST_ASSERT_FALSE(formatTopic("receiver-1", 2, topic, 10));
}
void test_sample_values_statuses_and_unknown_registry_entries() {
    Data data{};
    data.nextSeconds = 700000; // Clamped to Central's seven-day maximum.
    data.count = 5;
    data.readings[0] = reading(1, 1, 1, Status::Ok, -1250);
    data.readings[1] = reading(1, 2, 2, Status::Ok, 0);
    data.readings[2] = reading(2, 1, 1, Status::Error, 0);
    data.readings[3] = reading(3, 9, 7, Status::Skipped, 0);
    data.readings[4] = reading(4, 1, 1, Status::Ok, INT32_MIN);
    TEST_ASSERT_EQUAL_STRING(
        "{\"version\":1,\"source_id\":\"receiver-1\",\"device_id\":\"0000000000000002\","
        "\"sample_id\":\"000000000000000a.7\",\"expected_interval_seconds\":604800,"
        "\"readings\":["
        "{\"sensor_id\":\"sensor-1\",\"metric\":\"temperature\",\"value\":-1.250,"
        "\"unit\":\"degC\",\"status\":\"ok\"},"
        "{\"sensor_id\":\"sensor-1\",\"metric\":\"humidity\",\"value\":0.000,"
        "\"unit\":\"%\",\"status\":\"ok\"},"
        "{\"sensor_id\":\"sensor-2\",\"metric\":\"temperature\",\"unit\":\"degC\","
        "\"status\":\"error\"},"
        "{\"sensor_id\":\"sensor-3\",\"metric\":\"metric-9\",\"unit\":\"unit-7\","
        "\"status\":\"skipped\"},"
        "{\"sensor_id\":\"sensor-4\",\"metric\":\"temperature\",\"value\":-2147483.648,"
        "\"unit\":\"degC\",\"status\":\"ok\"}]}",
        format(queued(data)).c_str());
}
void test_invalid_samples_or_small_buffers_are_rejected() {
    Data data{};
    data.nextSeconds = 300;
    data.count = 1;
    data.readings[0] = reading(1, 1, 1, Status::Ok, 1);
    TEST_ASSERT_EQUAL_STRING("<rejected>", format(queued(data), "bad source").c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>", format(queued(data), nullptr).c_str());
    char small[40]{};
    size_t size = 1;
    TEST_ASSERT_FALSE(formatSample("receiver-1", queued(data), small, sizeof(small), size));
    TEST_ASSERT_EQUAL_size_t(0, size);
    TEST_ASSERT_FALSE(formatSample("receiver-1", queued(data), nullptr, 0, size));
    auto broken = data;
    broken.count = 0;
    TEST_ASSERT_EQUAL_STRING("<rejected>", format(queued(broken)).c_str());
    broken = data;
    broken.count = MaxReadings + 1;
    TEST_ASSERT_EQUAL_STRING("<rejected>", format(queued(broken)).c_str());
    broken = data;
    broken.nextSeconds = 0;
    TEST_ASSERT_EQUAL_STRING("<rejected>", format(queued(broken)).c_str());
    broken = data;
    broken.readings[0].status = Status(9);
    TEST_ASSERT_EQUAL_STRING("<rejected>", format(queued(broken)).c_str());
}

// --- Forwarder -------------------------------------------------------------------------
class TestClock final : public Clock {
public:
    uint32_t time = 1000;
    uint32_t nowMs() const override { return time; }
};
class TestPublisher final : public Publisher {
public:
    bool online = true;
    int nextId = 1;
    bool reject = false;
    std::deque<int> acks;
    std::deque<std::string> topics, payloads;
    bool connected() override { return online; }
    int publish(const char* topic, const char* payload, size_t size) override {
        TEST_ASSERT_EQUAL_size_t(std::strlen(payload), size);
        if (reject) return -1;
        topics.emplace_back(topic);
        payloads.emplace_back(payload);
        return nextId++;
    }
    bool acknowledged(int& id) override {
        if (acks.empty()) return false;
        id = acks.front();
        acks.pop_front();
        return true;
    }
};
struct ForwardRig {
    MemoryBlob blob;
    std::unique_ptr<PersistentStore> store = fixtures::mounted(blob, Role::Receiver);
    TestClock clock;
    TestPublisher publisher;
    Forwarder forwarder{publisher, clock, *store, "receiver-1"};
    ForwardRig() { TEST_ASSERT_TRUE(fixtures::enroll(*store)); }
    void receive(uint64_t counter) {
        Frame ack{};
        EXPECT_RESULT(Result::Ok,
                      cajui::receive(fixtures::binding(), fixtures::data(counter), *store, ack));
    }
};
void test_forwarder_removes_only_after_matching_puback() {
    ForwardRig rig;
    rig.forwarder.poll(true);
    TEST_ASSERT_TRUE(rig.publisher.payloads.empty()); // Empty queue: nothing to send.
    rig.receive(1);
    rig.receive(2);
    rig.forwarder.poll(false);
    TEST_ASSERT_TRUE(rig.publisher.payloads.empty()); // Radio busy: defer.
    rig.publisher.online = false;
    rig.forwarder.poll(true);
    TEST_ASSERT_TRUE(rig.publisher.payloads.empty());
    rig.publisher.online = true;
    rig.forwarder.poll(true);
    EXPECT_RESULT(ForwardState::Waiting, rig.forwarder.state());
    TEST_ASSERT_EQUAL_STRING("telemetry/v1/receiver-1/0000000000000002/samples",
                             rig.publisher.topics.back().c_str());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rig.publisher.payloads.back().find(
                                                 "\"sample_id\":\"000000000000000a.1\""));
    rig.publisher.acks.push_back(99); // Unrelated or late PUBACK.
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(2, rig.store->queued());
    rig.publisher.acks.push_back(1);
    rig.forwarder.poll(false); // Acks wait while the radio is busy.
    TEST_ASSERT_EQUAL_size_t(2, rig.store->queued());
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(1, rig.store->queued());
    TEST_ASSERT_EQUAL_UINT32(1, rig.forwarder.forwarded());
    EXPECT_RESULT(ForwardState::Idle, rig.forwarder.state());
    rig.forwarder.poll(true);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rig.publisher.payloads.back().find(
                                                 "\"sample_id\":\"000000000000000a.2\""));
    rig.publisher.acks.push_back(2);
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(0, rig.store->queued());
    // The removal is durable: a remounted store starts with an empty queue.
    auto reopened = fixtures::mounted(rig.blob, Role::Receiver);
    TEST_ASSERT_EQUAL_size_t(0, reopened->queued());
}
void test_forwarder_republishes_the_same_sample_after_loss() {
    ForwardRig rig;
    rig.receive(1);
    rig.forwarder.poll(true);
    const std::string first = rig.publisher.payloads.back();
    rig.clock.time += Forwarder::AckTimeoutMs; // PUBACK lost.
    rig.forwarder.poll(true);
    EXPECT_RESULT(ForwardState::Idle, rig.forwarder.state());
    TEST_ASSERT_EQUAL_UINT32(1, rig.forwarder.retries());
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(1, rig.publisher.payloads.size()); // Still backing off.
    rig.clock.time += Forwarder::RetryDelayMs;
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(2, rig.publisher.payloads.size());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), rig.publisher.payloads.back().c_str());
    rig.publisher.online = false; // Broker connection drops while waiting.
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_UINT32(2, rig.forwarder.retries());
    rig.publisher.online = true;
    rig.publisher.reject = true; // Client outbox refuses the publication.
    rig.clock.time += Forwarder::RetryDelayMs;
    rig.forwarder.poll(true);
    EXPECT_RESULT(ForwardState::Idle, rig.forwarder.state());
    rig.publisher.reject = false;
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(2, rig.publisher.payloads.size());
    rig.clock.time += Forwarder::RetryDelayMs;
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(3, rig.publisher.payloads.size());
    TEST_ASSERT_EQUAL_STRING(first.c_str(), rig.publisher.payloads.back().c_str());
    rig.publisher.acks.push_back(2); // The abandoned attempt's PUBACK arrives late.
    rig.publisher.acks.push_back(3);
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(0, rig.store->queued());
    TEST_ASSERT_EQUAL_UINT32(1, rig.forwarder.forwarded());
}
void test_forwarder_stops_on_storage_failure_and_handles_changed_front() {
    ForwardRig changed;
    changed.receive(1);
    changed.forwarder.poll(true);
    // Another owner removed the front: the PUBACK must not remove a different sample.
    EXPECT_RESULT(Result::Ok, changed.store->forwarded(2, 10, 1));
    changed.receive(2);
    changed.publisher.acks.push_back(1);
    changed.forwarder.poll(true);
    EXPECT_RESULT(ForwardState::Idle, changed.forwarder.state());
    TEST_ASSERT_EQUAL_size_t(1, changed.store->queued());
    TEST_ASSERT_EQUAL_UINT32(0, changed.forwarder.forwarded());

    ForwardRig failing;
    failing.receive(1);
    failing.forwarder.poll(true);
    failing.blob.failBefore = true;
    failing.publisher.acks.push_back(1);
    failing.forwarder.poll(true);
    EXPECT_RESULT(ForwardState::Failed, failing.forwarder.state());
    failing.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(1, failing.publisher.payloads.size());

    ForwardRig unhealthy;
    unhealthy.receive(1);
    unhealthy.blob.failBefore = true;
    EXPECT_RESULT(Result::StorageError, unhealthy.store->revoke(2, 10));
    unhealthy.forwarder.poll(true);
    EXPECT_RESULT(ForwardState::Failed, unhealthy.forwarder.state());

    MemoryBlob blob;
    auto store = fixtures::mounted(blob, Role::Receiver);
    TestClock clock;
    TestPublisher publisher;
    Forwarder invalid(publisher, clock, *store, "bad source");
    EXPECT_RESULT(ForwardState::Failed, invalid.state());
    Forwarder missing(publisher, clock, *store, nullptr);
    EXPECT_RESULT(ForwardState::Failed, missing.state());
}

void test_forwarder_switches_source_and_republishes() {
    ForwardRig rig;
    rig.receive(1);
    rig.forwarder.poll(true);
    TEST_ASSERT_FALSE(rig.forwarder.setSource("bad source"));
    TEST_ASSERT_FALSE(rig.forwarder.setSource(nullptr));
    EXPECT_RESULT(ForwardState::Waiting, rig.forwarder.state());
    TEST_ASSERT_TRUE(rig.forwarder.setSource("receiver-2"));
    EXPECT_RESULT(ForwardState::Idle, rig.forwarder.state());
    rig.publisher.acks.push_back(1); // PUBACK for the abandoned publication.
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(2, rig.publisher.payloads.size());
    TEST_ASSERT_EQUAL_STRING("telemetry/v1/receiver-2/0000000000000002/samples",
                             rig.publisher.topics.back().c_str());
    TEST_ASSERT_EQUAL_size_t(1, rig.store->queued());
    rig.publisher.acks.push_back(2);
    rig.forwarder.poll(true);
    TEST_ASSERT_EQUAL_size_t(0, rig.store->queued());
    TEST_ASSERT_TRUE(rig.forwarder.setSource("receiver-3")); // Idle: nothing abandoned.
}

// --- USB administration ----------------------------------------------------------------
void command(Provisioning& admin, const char* input, const char* expected, UNITY_LINE_TYPE line) {
    char out[ReplyCapacity]{};
    UNITY_TEST_ASSERT(admin.execute(input, std::strlen(input), out, sizeof(out)), line,
                      "Reply buffer rejected");
    UNITY_TEST_ASSERT_EQUAL_STRING(expected, out, line, nullptr);
}
#define COMMAND(admin, input, expected) command(admin, input, expected, __LINE__)
std::string hexOf(const char* text) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const char* c = text; *c; ++c) {
        out += digits[uint8_t(*c) >> 4];
        out += digits[uint8_t(*c) & 15];
    }
    return out;
}
std::string set(const char* field, const char* value) {
    return std::string("CJ1 UPLINKSET 0000000000000001 ") + field + " " + hexOf(value);
}
void test_usb_uplink_settings_are_staged_saved_and_never_echoed() {
    MemoryBlob snapshot, settings;
    auto store = fixtures::mounted(snapshot, Role::Receiver);
    Provisioning admin(*store, 1, &settings);
    COMMAND(admin, "CJ1 UPLINKINFO 0000000000000001", "CJ1 OK UPLINKINFO 0");
    COMMAND(admin, "CJ1 UPLINKSAVE 0000000000000001", "CJ1 ERR INVALID"); // Incomplete.
    const char* fields[][2] = {{"ssid", "Bench network"}, {"wifipass", "correct horse"},
                               {"host", "192.168.1.20"},  {"port", "1883"},
                               {"user", "receiver-1"},    {"pass", "broker secret"}};
    for (auto& field : fields) COMMAND(admin, set(field[0], field[1]).c_str(), "CJ1 OK UPLINKSET");
    TEST_ASSERT_EQUAL_size_t(0, settings.writes);
    COMMAND(admin, "CJ1 UPLINKSAVE 0000000000000001", "CJ1 OK UPLINKSAVE");
    COMMAND(admin, "CJ1 UPLINKINFO 0000000000000001",
            "CJ1 OK UPLINKINFO 1 192.168.1.20 1883 receiver-1");
    COMMAND(admin, "CJ1 UPLINKSAVE 0000000000000001", "CJ1 ERR INVALID"); // Staging wiped.
    UplinkConfig stored{};
    EXPECT_RESULT(ReadResult::Ok, loadUplink(settings, stored));
    TEST_ASSERT_EQUAL_STRING("correct horse", stored.wifiPassword);
    for (auto& field : fields) COMMAND(admin, set(field[0], field[1]).c_str(), "CJ1 OK UPLINKSET");
    settings.failBefore = true;
    COMMAND(admin, "CJ1 UPLINKSAVE 0000000000000001", "CJ1 ERR STORAGE");
    settings.failBefore = false;
    settings.failRead = true;
    COMMAND(admin, "CJ1 UPLINKINFO 0000000000000001", "CJ1 ERR STORAGE");
}
void test_usb_uplink_rejects_bad_values_roles_and_devices() {
    MemoryBlob snapshot, settings;
    auto store = fixtures::mounted(snapshot, Role::Receiver);
    Provisioning admin(*store, 1, &settings);
    const std::string invalid[] = {
        set("port", "0"),
        set("port", "65536"),
        set("port", "18a3"),
        set("port", "123456"),
        set("unknown", "value"),
        set("user", std::string(65, 'u').c_str()),
        "CJ1 UPLINKSET 0000000000000001 ssid 4",    // Odd hex length.
        "CJ1 UPLINKSET 0000000000000001 ssid 4A",   // Upper-case hex.
        "CJ1 UPLINKSET 0000000000000001 ssid 4100", // Embedded NUL.
        "CJ1 UPLINKSET 0000000000000002 ssid 41",   // Wrong device.
        "CJ1 UPLINKSET 0000000000000001 ssid",      // Missing value.
        "CJ1 UPLINKINFO 0000000000000001 extra",
        "CJ1 UPLINKFOO 0000000000000001",
    };
    for (const auto& input : invalid) {
        UNITY_SET_DETAIL(input.c_str());
        COMMAND(admin, input.c_str(), "CJ1 ERR INVALID");
    }
    COMMAND(admin, set("port", "65535").c_str(), "CJ1 OK UPLINKSET");
    Provisioning noBlob(*store);
    COMMAND(noBlob, "CJ1 UPLINKINFO 0000000000000001", "CJ1 ERR INVALID");
    MemoryBlob txSnapshot;
    auto transmitter = fixtures::mounted(txSnapshot, Role::Transmitter);
    Provisioning tx(*transmitter, 1, &settings);
    COMMAND(tx, "CJ1 UPLINKINFO 0000000000000002", "CJ1 ERR INVALID");
}
} // namespace

void runUplinkTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_uplink_settings_round_trip_and_fail_closed);
    RUN_TEST(test_uplink_settings_are_validated_before_saving);
    RUN_TEST(test_sample_matches_the_central_mqtt_contract);
    RUN_TEST(test_sample_values_statuses_and_unknown_registry_entries);
    RUN_TEST(test_invalid_samples_or_small_buffers_are_rejected);
    RUN_TEST(test_forwarder_removes_only_after_matching_puback);
    RUN_TEST(test_forwarder_republishes_the_same_sample_after_loss);
    RUN_TEST(test_forwarder_stops_on_storage_failure_and_handles_changed_front);
    RUN_TEST(test_forwarder_switches_source_and_republishes);
    RUN_TEST(test_usb_uplink_settings_are_staged_saved_and_never_echoed);
    RUN_TEST(test_usb_uplink_rejects_bad_values_roles_and_devices);
}
