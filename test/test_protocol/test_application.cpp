#include <unity.h>
#include <cmath>
#include "cajui_application.h"
#include "storage_support.h"
#include "assertions.h"

namespace {
using namespace cajui;
class TestClock final : public Clock {
public:
    uint32_t time = 0;
    uint32_t nowMs() const override { return time; }
};
class TestRadio final : public ReceiverRadio {
public:
    Frame input{}, sent{};
    bool listenOk = true, sendOk = true, sleepOk = true;
    ReceiveStatus rx = ReceiveStatus::Empty;
    TransmitStatus tx = TransmitStatus::Pending;
    size_t sends = 0, sleeps = 0;
    PersistentStore* store = nullptr;
    bool listen() override { return listenOk; }
    bool startChannelCheck() override { return false; }
    ChannelStatus channelStatus() override { return ChannelStatus::Error; }
    bool startTransmit(const Frame& frame) override {
        // The observable ACK is never started before the sample exists durably.
        TEST_ASSERT_NOT_NULL(store);
        TEST_ASSERT_GREATER_THAN(0, store->queued());
        sent = frame;
        ++sends;
        return sendOk;
    }
    TransmitStatus transmitStatus(uint32_t& completed) override {
        completed = 0;
        return tx;
    }
    ReceiveStatus receive(Frame& frame) override {
        frame = input;
        const auto result = rx;
        rx = ReceiveStatus::Empty;
        return result;
    }
    bool sleep() override {
        ++sleeps;
        return sleepOk;
    }
    void deliver(const Frame& frame) {
        input = frame;
        rx = ReceiveStatus::Received;
    }
};
struct Rig {
    fixtures::MemoryBlob blob;
    std::unique_ptr<PersistentStore> store = fixtures::mounted(blob, Role::Receiver);
    TestClock clock;
    TestRadio radio;
    ReceiverController controller{radio, clock, *store};
    Rig() {
        radio.store = store.get();
        TEST_ASSERT_TRUE(fixtures::enroll(*store));
    }
    void start() { TEST_ASSERT_TRUE(controller.start()); }
    void pollData(uint64_t counter = 1) {
        radio.deliver(fixtures::data(counter));
        controller.poll();
    }
    void complete() {
        radio.tx = TransmitStatus::Complete;
        controller.poll();
    }
};
void test_receiver_routes_only_enrolled_authenticated_data() {
    Rig r;
    r.controller.poll();
    r.start();
    TEST_ASSERT_FALSE(r.controller.start());
    r.controller.poll();
    const auto writes = r.blob.writes;
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto frame = fixtures::data(1, scenario == 0 ? 3 : 2);
        if (scenario == 1) frame.bytes[6] ^= 1; // same node, foreign network
        if (scenario == 2) frame.bytes[frame.size - 1] ^= 1;
        if (scenario == 3) frame.size = SIZE_MAX;
        r.radio.deliver(frame);
        r.controller.poll();
        TEST_ASSERT_EQUAL_UINT(0, r.radio.sends);
        TEST_ASSERT_EQUAL_UINT(writes, r.blob.writes);
    }
    r.pollData();
    TEST_ASSERT_EQUAL_UINT(1, r.store->queued());
    EXPECT_RESULT(Result::Ok, r.controller.lastResult());
    Message ack{};
    EXPECT_RESULT(Result::Ok, open(fixtures::binding(), r.radio.sent, ack));
    TEST_ASSERT_EQUAL_INT(int(Type::Ack), int(ack.type));
    r.complete();
    const auto firstAck = r.radio.sent;
    r.pollData();
    EXPECT_RESULT(Result::Duplicate, r.controller.lastResult());
    TEST_ASSERT_TRUE(sameFrame(firstAck, r.radio.sent));
    TEST_ASSERT_EQUAL_UINT(1, r.store->queued());
    r.complete();
    r.pollData(2);
    r.complete();
    r.pollData(1);
    EXPECT_RESULT(Result::Replay, r.controller.lastResult());
    TEST_ASSERT_EQUAL_UINT(3, r.radio.sends);
}
void test_receiver_storage_failure_never_acknowledges() {
    Rig r;
    r.start();
    r.blob.failBefore = true;
    r.pollData();
    EXPECT_RESULT(Result::StorageError, r.controller.lastResult());
    TEST_ASSERT_EQUAL_INT(int(ReceiverState::Failed), int(r.controller.state()));
    TEST_ASSERT_EQUAL_UINT(0, r.radio.sends);
    TEST_ASSERT_EQUAL_UINT(1, r.radio.sleeps);
    r.controller.poll();
    TEST_ASSERT_FALSE(r.controller.start());
}
void test_receiver_full_queue_preserves_receipt_and_reacks_duplicate() {
    Rig r;
    r.start();
    for (uint64_t counter = 1; counter <= 128; ++counter) {
        r.pollData(counter);
        r.complete();
    }
    r.pollData(129);
    EXPECT_RESULT(Result::Full, r.controller.lastResult());
    TEST_ASSERT_EQUAL_UINT(128, r.radio.sends);
    r.pollData(128);
    EXPECT_RESULT(Result::Duplicate, r.controller.lastResult());
    TEST_ASSERT_EQUAL_UINT(129, r.radio.sends);
}
void test_receiver_radio_failures_and_timeout_are_terminal() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        Rig r;
        r.start();
        if (scenario == 0) {
            r.radio.rx = ReceiveStatus::Error;
            r.controller.poll();
        } else {
            r.radio.sendOk = scenario != 1;
            r.pollData();
            if (scenario > 1) {
                r.controller.poll(); // still pending
                TEST_ASSERT_EQUAL_INT(int(ReceiverState::Acknowledging), int(r.controller.state()));
                if (scenario == 2)
                    r.radio.tx = TransmitStatus::Error;
                else
                    r.clock.time = 3000;
                if (scenario == 4) r.radio.sleepOk = false;
                r.controller.poll();
            }
        }
        TEST_ASSERT_EQUAL_INT(int(ReceiverState::Failed), int(r.controller.state()));
        TEST_ASSERT_EQUAL_UINT(1, r.radio.sleeps);
    }
}
void test_receiver_start_requires_healthy_receiver_storage_and_radio() {
    // A receiver without bindings starts (scenario 2): radio pairing creates the first one.
    for (int scenario = 0; scenario < 4; ++scenario) {
        fixtures::MemoryBlob blob;
        auto store = fixtures::mounted(blob, scenario == 1 ? Role::Transmitter : Role::Receiver);
        if (scenario != 2) TEST_ASSERT_TRUE(fixtures::enroll(*store));
        if (scenario == 0) {
            blob.failRead = true;
            TEST_ASSERT_FALSE(store->mount());
        }
        TestRadio radio;
        radio.listenOk = scenario != 3;
        TestClock clock;
        ReceiverController controller(radio, clock, *store);
        SCENARIO(scenario);
        if (scenario == 2) {
            TEST_ASSERT_TRUE(controller.start());
            continue;
        }
        TEST_ASSERT_FALSE(controller.start());
        TEST_ASSERT_EQUAL_INT(int(ReceiverState::Failed), int(controller.state()));
    }
}
void test_routing_hint_is_bounded_and_never_authentication() {
    const auto valid = fixtures::data(1);
    TEST_ASSERT_EQUAL_UINT64(2, untrustedDataNode(valid));
    for (size_t size = 0; size < valid.size; ++size) {
        auto frame = valid;
        frame.size = size;
        TEST_ASSERT_EQUAL_UINT64(0, untrustedDataNode(frame));
    }
    for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(30), size_t(31)}) {
        auto frame = valid;
        frame.bytes[offset] ^= 4;
        TEST_ASSERT_EQUAL_UINT64(0, untrustedDataNode(frame));
    }
    auto malformed = valid;
    malformed.size = MaxFrame + 1;
    TEST_ASSERT_EQUAL_UINT64(0, untrustedDataNode(malformed));
    malformed = valid;
    malformed.size++;
    malformed.bytes[31]++;
    TEST_ASSERT_EQUAL_UINT64(0, untrustedDataNode(malformed));
    // A forged tag can have a plausible routing hint. It is not an authenticated identity.
    auto forged = valid;
    forged.bytes[forged.size - 1] ^= 1;
    TEST_ASSERT_EQUAL_UINT64(2, untrustedDataNode(forged));
}
void test_climate_values_preserve_zero_and_flag_invalid_measurements() {
    auto sample = climateSample(-12.345f, 0, 300);
    TEST_ASSERT_EQUAL_UINT(2, sample.count);
    TEST_ASSERT_EQUAL_UINT(0, sample.batteryMv);
    TEST_ASSERT_EQUAL_INT32(-12345, sample.readings[0].milliValue);
    TEST_ASSERT_EQUAL_INT(int(Status::Ok), int(sample.readings[1].status));
    const float values[] = {NAN, INFINITY, -INFINITY, -41, 81};
    for (float value : values) {
        sample = climateSample(value, value, 300);
        TEST_ASSERT_EQUAL_INT(int(Status::Error), int(sample.readings[0].status));
        TEST_ASSERT_EQUAL_INT32(0, sample.readings[0].milliValue);
    }
    for (float value : {-1.0f, 101.0f, NAN}) {
        sample = climateSample(0, value, 300);
        TEST_ASSERT_EQUAL_INT(int(Status::Error), int(sample.readings[1].status));
        TEST_ASSERT_EQUAL_INT32(0, sample.readings[1].milliValue);
    }
    sample = climateSample(-40, 100, 300);
    TEST_ASSERT_EQUAL_INT32(-40000, sample.readings[0].milliValue);
    TEST_ASSERT_EQUAL_INT32(100000, sample.readings[1].milliValue);
    sample = climateSample(80, 0, 300);
    TEST_ASSERT_EQUAL_INT(int(Status::Ok), int(sample.readings[0].status));
}
}
void runApplicationTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_receiver_routes_only_enrolled_authenticated_data);
    RUN_TEST(test_receiver_storage_failure_never_acknowledges);
    RUN_TEST(test_receiver_full_queue_preserves_receipt_and_reacks_duplicate);
    RUN_TEST(test_receiver_radio_failures_and_timeout_are_terminal);
    RUN_TEST(test_receiver_start_requires_healthy_receiver_storage_and_radio);
    RUN_TEST(test_routing_hint_is_bounded_and_never_authentication);
    RUN_TEST(test_climate_values_preserve_zero_and_flag_invalid_measurements);
}
