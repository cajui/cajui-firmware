#include <unity.h>
#include "cajui_runtime.h"
#include "storage_support.h"
#include <vector>

namespace {
using namespace cajui;
using fixtures::binding;
using fixtures::sample;

class FakeClock final : public Clock {
public:
    uint32_t time = 0;
    uint32_t nowMs() const override { return time; }
    void advance(uint32_t ms) { time += ms; }
};
class FakeJitter final : public Jitter {
public:
    bool fail = false, high = false;
    int outside = 0;
    std::vector<uint32_t> bounds;
    bool between(uint32_t minimum, uint32_t maximum, uint32_t& value) override {
        bounds.push_back(minimum); bounds.push_back(maximum);
        value = outside > 0 ? maximum + 1 : outside < 0 ? minimum - 1 : high ? maximum : minimum;
        return !fail;
    }
};
class FakeCounter final : public CounterStore {
public:
    uint64_t last = 0;
    bool fail = false;
    FakeClock* clock = nullptr;
    uint32_t latencyMs = 0;
    bool reserve(const Binding&, uint64_t& out) override {
        if (clock) clock->advance(latencyMs);
        if (fail) return false;
        out = ++last; return true;
    }
};
class FakeRadio final : public Radio {
public:
    ChannelStatus channel = ChannelStatus::Clear;
    TransmitStatus tx = TransmitStatus::Complete;
    ReceiveStatus rx = ReceiveStatus::Empty;
    bool cadStart = true, txStart = true, sleepOk = true, failOneSleep = false;
    unsigned cadCalls = 0, readCalls = 0, sleepCalls = 0;
    uint32_t completedAt = 0;
    Frame incoming{};
    const Frame* retained = nullptr;
    std::vector<Frame> sent;
    bool startChannelCheck() override { ++cadCalls; return cadStart; }
    ChannelStatus channelStatus() override { return channel; }
    bool startTransmit(const Frame& frame) override {
        retained = &frame; sent.push_back(frame); return txStart;
    }
    TransmitStatus transmitStatus(uint32_t& time) override { time = completedAt; return tx; }
    ReceiveStatus receive(Frame& out) override { ++readCalls; out = incoming; return rx; }
    bool sleep() override {
        ++sleepCalls;
        if (failOneSleep) { failOneSleep = false; return false; }
        if (sleepOk) retained = nullptr;
        return sleepOk;
    }
};
struct Rig {
    FakeClock clock;
    FakeJitter jitter;
    FakeCounter counter;
    FakeRadio radio;
    SendController controller;
    explicit Rig(const SendPolicy& p = SendPolicy{}) : controller(radio, clock, jitter, p) {}
    void start() {
        TEST_ASSERT_EQUAL_INT(int(StartResult::Started), int(controller.start(binding(), sample(), counter)));
    }
    void enterChannelCheck() {
        controller.poll(); // Starting -> initial jitter
        controller.poll(); // Zero initial jitter -> CAD
        TEST_ASSERT_EQUAL_INT(int(SendState::CheckingChannel), int(controller.state()));
    }
    void transmit() {
        controller.poll(); // Clear channel -> TX
        TEST_ASSERT_EQUAL_INT(int(SendState::Transmitting), int(controller.state()));
        radio.completedAt = clock.time;
        controller.poll(); // TX done -> RX
        TEST_ASSERT_EQUAL_INT(int(SendState::AwaitingAck), int(controller.state()));
    }
    void retry(uint32_t backoff) {
        clock.advance(1500); controller.poll();
        clock.advance(backoff); controller.poll();
        transmit();
    }
};
void completion(Completion expected, const Rig& rig) {
    TEST_ASSERT_EQUAL_INT(int(expected), int(rig.controller.report().completion));
    TEST_ASSERT_FALSE(rig.controller.active());
    TEST_ASSERT_TRUE(rig.controller.report().radioSleeping);
    TEST_ASSERT_NULL(rig.radio.retained);
}
Frame ackFor(const Frame& frame, const Binding& b = binding()) {
    Message data{}; TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(open(b, frame, data)));
    Message ack{}; ack.type = Type::Ack; ack.counter = data.counter;
    std::memcpy(ack.dataTag.data(), frame.bytes.data() + frame.size - TagSize, TagSize);
    Frame out{}; TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(seal(b, ack, out))); return out;
}
void test_runtime_idle_and_success_reuse() {
    Rig r;
    TEST_ASSERT_EQUAL_INT(int(SendState::Idle), int(r.controller.state()));
    r.controller.poll(); r.controller.cancel(); TEST_ASSERT_EQUAL_UINT(0, r.radio.sleepCalls);
    r.start(); TEST_ASSERT_EQUAL_UINT64(1, r.counter.last);
    TEST_ASSERT_EQUAL_INT(int(StartResult::Busy), int(r.controller.start(binding(), sample(), r.counter)));
    r.enterChannelCheck(); r.transmit(); r.controller.poll(); // No incoming frame.
    r.radio.incoming = ackFor(r.radio.sent[0]); r.radio.rx = ReceiveStatus::Received;
    r.controller.poll(); completion(Completion::Acknowledged, r);
    TEST_ASSERT_EQUAL_UINT8(1, r.controller.report().attempts);
    auto sleeps = r.radio.sleepCalls; r.controller.poll(); r.controller.cancel();
    TEST_ASSERT_EQUAL_UINT(sleeps, r.radio.sleepCalls);
    r.start(); TEST_ASSERT_EQUAL_UINT64(2, r.counter.last); r.controller.cancel();
    completion(Completion::Cancelled, r);
}
void test_runtime_jitter_and_busy_channel_are_bounded() {
    Rig r; r.jitter.high = true; r.start(); r.controller.poll();
    r.clock.advance(499); r.controller.poll(); TEST_ASSERT_EQUAL_UINT(0, r.radio.cadCalls);
    r.clock.advance(1); r.controller.poll(); r.radio.channel = ChannelStatus::Busy; r.controller.poll();
    TEST_ASSERT_EQUAL_UINT(0, r.radio.sent.size());
    TEST_ASSERT_EQUAL_UINT32(100, r.jitter.bounds[2]);
    TEST_ASSERT_EQUAL_UINT32(500, r.jitter.bounds[3]);
    r.clock.advance(9499); r.controller.poll(); // One ms remains: CAD may start, no TX.
    r.clock.advance(1); r.controller.poll(); completion(Completion::Deadline, r);
    TEST_ASSERT_EQUAL_UINT(0, r.radio.sent.size());
}
void test_runtime_retries_identical_frames_then_exhausts() {
    Rig r; r.start(); r.enterChannelCheck(); r.transmit();
    r.retry(100); r.retry(200);
    TEST_ASSERT_EQUAL_UINT(3, r.radio.sent.size());
    TEST_ASSERT_TRUE(sameFrame(r.radio.sent[0], r.radio.sent[1]));
    TEST_ASSERT_TRUE(sameFrame(r.radio.sent[0], r.radio.sent[2]));
    TEST_ASSERT_EQUAL_UINT64(1, r.counter.last);
    TEST_ASSERT_EQUAL_UINT32(200, r.jitter.bounds[4]);
    TEST_ASSERT_EQUAL_UINT32(1000, r.jitter.bounds[5]);
    r.clock.advance(1499); r.controller.poll(); TEST_ASSERT_TRUE(r.controller.active());
    r.clock.advance(1); r.controller.poll(); completion(Completion::AttemptsExhausted, r);
    TEST_ASSERT_EQUAL_UINT8(3, r.controller.report().attempts);
}
void test_runtime_lost_ack_replays_without_duplicate_commit() {
    Rig r; fixtures::MemoryBlob blob; auto store = fixtures::mounted(blob, Role::Receiver);
    TEST_ASSERT_TRUE(fixtures::enroll(*store));
    r.start(); r.enterChannelCheck(); r.transmit();
    Frame firstAck{}; TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(receive(binding(), r.radio.sent[0], *store, firstAck)));
    r.retry(100);
    TEST_ASSERT_EQUAL_INT(int(Result::Duplicate), int(receive(binding(), r.radio.sent[1], *store, r.radio.incoming)));
    TEST_ASSERT_TRUE(sameFrame(firstAck, r.radio.incoming));
    TEST_ASSERT_EQUAL_UINT(1, store->queued());
    r.radio.rx = ReceiveStatus::Received; r.controller.poll(); completion(Completion::Acknowledged, r);
}
void test_runtime_storage_failures_never_produce_transmission_or_ack() {
    Rig r; fixtures::MemoryBlob blob; auto tx = fixtures::mounted(blob);
    TEST_ASSERT_TRUE(fixtures::enroll(*tx)); blob.failAfter = true;
    TEST_ASSERT_EQUAL_INT(int(StartResult::ProtocolRejected), int(r.controller.start(binding(), sample(), *tx)));
    TEST_ASSERT_EQUAL_INT(int(Result::StorageError), int(r.controller.protocolResult()));
    r.controller.poll(); TEST_ASSERT_EQUAL_UINT(0, r.radio.cadCalls); TEST_ASSERT_TRUE(r.radio.sent.empty());
    blob.failAfter = false; tx = fixtures::mounted(blob);
    TEST_ASSERT_EQUAL_INT(int(StartResult::Started), int(r.controller.start(binding(), sample(), *tx)));
    r.enterChannelCheck(); r.transmit();
    Message decoded{}; TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(open(binding(), r.radio.sent[0], decoded)));
    TEST_ASSERT_EQUAL_UINT64(2, decoded.counter); // Ambiguous reservation was consumed.
    fixtures::MemoryBlob rxBlob; auto rx = fixtures::mounted(rxBlob, Role::Receiver);
    TEST_ASSERT_TRUE(fixtures::enroll(*rx)); rxBlob.failBefore = true;
    Frame ack{}; TEST_ASSERT_EQUAL_INT(int(Result::StorageError), int(receive(binding(), r.radio.sent[0], *rx, ack)));
    TEST_ASSERT_EQUAL_UINT(0, ack.size);
    r.retry(100); r.retry(200); r.clock.advance(1500); r.controller.poll();
    completion(Completion::AttemptsExhausted, r);
}
void test_runtime_bad_acks_do_not_extend_the_window() {
    Rig r; r.start(); r.enterChannelCheck(); r.transmit(); r.radio.rx = ReceiveStatus::Received;
    const Frame good = ackFor(r.radio.sent[0]);
    r.radio.incoming = good; r.radio.incoming.bytes[50] ^= 1; r.controller.poll();
    r.radio.incoming = r.radio.sent[0]; r.controller.poll(); // Authenticated DATA is not ACK.
    r.radio.incoming = ackFor(fixtures::data(2)); r.controller.poll(); // Wrong sample.
    r.radio.incoming = ackFor(fixtures::data(1, 3), binding(3)); r.controller.poll(); // Wrong node.
    r.radio.incoming = ackFor(fixtures::data(1, 2, 2), binding(2, 2)); r.controller.poll(); // Wrong key.
    Message wrong{}; wrong.type = Type::Ack; wrong.counter = 1;
    TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(seal(binding(), wrong, r.radio.incoming)));
    r.controller.poll(); // Right key/counter but wrong DATA tag.
    r.radio.incoming.size = SIZE_MAX; r.controller.poll();
    TEST_ASSERT_EQUAL_UINT32(7, r.controller.report().rejectedAcks);
    const auto calls = r.radio.readCalls;
    r.clock.advance(1500); r.radio.incoming = good; r.controller.poll();
    TEST_ASSERT_EQUAL_UINT(calls, r.radio.readCalls); // ACK at deadline is not processed.
    TEST_ASSERT_EQUAL_INT(int(SendState::Waiting), int(r.controller.state()));
    r.controller.cancel(); completion(Completion::Cancelled, r);
}
void test_runtime_persistence_latency_counts_toward_deadline() {
    Rig r; r.counter.clock = &r.clock; r.counter.latencyMs = 10000;
    r.start(); r.controller.poll(); completion(Completion::Deadline, r);
    TEST_ASSERT_EQUAL_UINT64(1, r.counter.last);
    TEST_ASSERT_EQUAL_UINT(0, r.radio.cadCalls);
    TEST_ASSERT_TRUE(r.radio.sent.empty());
}
void test_runtime_stop_failure_preserves_confirmed_delivery() {
    Rig r; r.start(); r.enterChannelCheck(); r.transmit();
    r.radio.incoming = ackFor(r.radio.sent[0]); r.radio.rx = ReceiveStatus::Received;
    r.radio.sleepOk = false; r.controller.poll();
    TEST_ASSERT_EQUAL_INT(int(Completion::Acknowledged), int(r.controller.report().completion));
    TEST_ASSERT_FALSE(r.controller.report().radioSleeping);
    TEST_ASSERT_EQUAL_INT(int(StartResult::RadioUnavailable), int(r.controller.start(binding(), sample(), r.counter)));
    r.radio.sleepOk = true; TEST_ASSERT_TRUE(r.radio.sleep());
}
void test_runtime_deadline_overrides_queued_valid_ack() {
    SendPolicy p; p.cycleTimeoutMs = 4000; Rig r(p);
    r.start(); r.enterChannelCheck(); r.transmit();
    r.radio.incoming = ackFor(r.radio.sent[0]); r.radio.rx = ReceiveStatus::Received;
    r.clock.advance(4000); r.controller.poll(); completion(Completion::Deadline, r);
    TEST_ASSERT_EQUAL_UINT(0, r.radio.readCalls);
}
void test_runtime_time_wrap_and_hardware_completion_timestamp() {
    Rig r; r.clock.time = UINT32_MAX - 100; r.start(); r.enterChannelCheck();
    r.controller.poll(); // TX started before clock rollover.
    r.clock.advance(200); r.radio.completedAt = r.clock.time; r.clock.advance(1499);
    r.controller.poll(); // Delayed observation of TX completion must not restart ACK timer.
    r.clock.advance(1); r.radio.rx = ReceiveStatus::Received; r.radio.incoming = ackFor(r.radio.sent[0]);
    r.controller.poll(); TEST_ASSERT_EQUAL_INT(int(SendState::Waiting), int(r.controller.state()));
    TEST_ASSERT_EQUAL_UINT(0, r.radio.readCalls);
    r.clock.advance(100); r.controller.poll(); r.transmit();
    r.radio.incoming = ackFor(r.radio.sent[1]); r.controller.poll(); completion(Completion::Acknowledged, r);
}
void test_runtime_pending_operations_have_timeouts() {
    for (int stage = 0; stage < 2; ++stage) {
        Rig r; r.start(); r.enterChannelCheck();
        if (stage == 0) r.radio.channel = ChannelStatus::Pending;
        else { r.controller.poll(); r.radio.tx = TransmitStatus::Pending; }
        r.controller.poll(); TEST_ASSERT_TRUE(r.controller.active());
        r.clock.advance(stage == 0 ? 1000 : 3000); r.controller.poll();
        completion(Completion::RadioTimeout, r);
    }
}
void test_runtime_driver_failures_stop_the_cycle() {
    for (int stage = 0; stage < 7; ++stage) {
        Rig r; r.start(); r.controller.poll();
        if (stage == 0) r.radio.cadStart = false;
        r.controller.poll();
        if (stage > 0) {
            if (stage == 1) r.radio.channel = ChannelStatus::Error;
            if (stage == 2) r.radio.txStart = false;
            r.controller.poll();
        }
        if (stage > 2) {
            if (stage == 3) r.radio.tx = TransmitStatus::Error;
            if (stage == 4) r.radio.completedAt = 1; // Future completion.
            if (stage == 5) r.radio.completedAt = UINT32_MAX; // Stale completion.
            r.controller.poll();
        }
        if (stage == 6) { r.radio.rx = ReceiveStatus::Error; r.controller.poll(); }
        completion(Completion::RadioError, r);
    }
}
void test_runtime_sleep_failure_retains_frame_and_blocks_reuse() {
    Rig r; r.start(); r.enterChannelCheck(); r.transmit();
    r.radio.sleepOk = false; r.controller.cancel();
    TEST_ASSERT_EQUAL_INT(int(Completion::Cancelled), int(r.controller.report().completion));
    TEST_ASSERT_FALSE(r.controller.report().radioSleeping);
    TEST_ASSERT_NOT_NULL(r.radio.retained);
    TEST_ASSERT_TRUE(sameFrame(*r.radio.retained, r.radio.sent[0]));
    TEST_ASSERT_EQUAL_INT(int(StartResult::RadioUnavailable), int(r.controller.start(binding(), sample(), r.counter)));
    TEST_ASSERT_EQUAL_UINT64(1, r.counter.last);
    // The adapter owner must recover/quiesce the radio before destroying the controller.
    r.radio.sleepOk = true; TEST_ASSERT_TRUE(r.radio.sleep());
    Rig initial; initial.start(); initial.radio.failOneSleep = true; initial.controller.poll();
    TEST_ASSERT_EQUAL_INT(int(Completion::RadioError), int(initial.controller.report().completion));
    TEST_ASSERT_FALSE(initial.controller.report().radioSleeping);
    TEST_ASSERT_EQUAL_UINT(1, initial.radio.sleepCalls); // No automatic retry after uncertain stop.
    TEST_ASSERT_EQUAL_INT(int(StartResult::RadioUnavailable),
        int(initial.controller.start(binding(), sample(), initial.counter)));
}
void test_runtime_random_failure_or_out_of_range_stops() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        Rig r; r.start();
        if (scenario == 0) r.jitter.fail = true;
        else if (scenario == 1) r.jitter.outside = 1;
        else {
            r.enterChannelCheck(); r.radio.channel = ChannelStatus::Busy; r.jitter.outside = -1;
        }
        r.controller.poll(); completion(Completion::RandomError, r);
        TEST_ASSERT_TRUE(r.radio.sent.empty());
    }
}
void test_runtime_rejects_invalid_policies_and_samples_before_reserving() {
    for (int i = 0; i < 14; ++i) {
        SendPolicy p;
        switch (i) {
        case 0: p.cycleTimeoutMs = 0; break;
        case 1: p.cycleTimeoutMs = UINT32_MAX; break;
        case 2: p.initialJitterMs = p.cycleTimeoutMs; break;
        case 3: p.ackTimeoutMs = 0; break;
        case 4: p.ackTimeoutMs = p.cycleTimeoutMs; break;
        case 5: p.channelTimeoutMs = 0; break;
        case 6: p.channelTimeoutMs = p.cycleTimeoutMs; break;
        case 7: p.transmitTimeoutMs = 0; break;
        case 8: p.transmitTimeoutMs = p.cycleTimeoutMs; break;
        case 9: p.firstBackoffMinMs = 0; break;
        case 10: p.firstBackoffMinMs = p.firstBackoffMaxMs + 1; break;
        case 11: p.firstBackoffMaxMs = p.cycleTimeoutMs; break;
        case 12: p.secondBackoffMinMs = 0; break;
        case 13: p.secondBackoffMinMs = p.secondBackoffMaxMs + 1; break;
        }
        Rig r(p); TEST_ASSERT_EQUAL_INT(int(StartResult::InvalidPolicy), int(r.controller.start(binding(), sample(), r.counter)));
        TEST_ASSERT_EQUAL_UINT64(0, r.counter.last);
    }
    SendPolicy p; p.secondBackoffMaxMs = p.cycleTimeoutMs; Rig bad(p);
    TEST_ASSERT_EQUAL_INT(int(StartResult::InvalidPolicy), int(bad.controller.start(binding(), sample(), bad.counter)));
    Rig r; auto data = sample(); data.count = 0;
    TEST_ASSERT_EQUAL_INT(int(StartResult::ProtocolRejected), int(r.controller.start(binding(), data, r.counter)));
    TEST_ASSERT_EQUAL_INT(int(Result::Invalid), int(r.controller.protocolResult()));
    TEST_ASSERT_EQUAL_UINT64(0, r.counter.last);
    auto b = binding(); b.active = false;
    TEST_ASSERT_EQUAL_INT(int(StartResult::ProtocolRejected), int(r.controller.start(b, sample(), r.counter)));
    TEST_ASSERT_EQUAL_INT(int(Result::Unauthorized), int(r.controller.protocolResult()));
}
void test_runtime_cancel_each_phase_consumes_counter_without_reuse() {
    for (int phase = 0; phase < 5; ++phase) {
        Rig r; r.start();
        for (int i = 0; i < phase; ++i) r.controller.poll();
        r.controller.cancel(); completion(Completion::Cancelled, r);
        auto count = r.radio.sent.size(); r.controller.poll(); TEST_ASSERT_EQUAL_UINT(count, r.radio.sent.size());
        r.start(); TEST_ASSERT_EQUAL_UINT64(2, r.counter.last); r.controller.cancel();
    }
}
void test_codec_matches_pre_refactor_wire_fixture() {
    const uint8_t expected[] = {
        0x43,0x4a,0x4c,0x52,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2a,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
        0x00,0x11,0xe3,0x30,0xa6,0x91,0xf0,0x79,0x7a,0xcd,0xcf,0xac,0x2b,0xc4,0x6c,0xa5,
        0xed,0xcf,0xeb,0x02,0x90,0x30,0x9f,0xc1,0xec,0x18,0xdf,0xca,0xd0,0x5f,0x45,0x3e,
        0xc9,0x87,0x9a
    };
    auto frame = fixtures::data(1);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), frame.size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame.bytes.data(), sizeof(expected));
}
}
void runRuntimeTests() {
    RUN_TEST(test_runtime_idle_and_success_reuse);
    RUN_TEST(test_runtime_jitter_and_busy_channel_are_bounded);
    RUN_TEST(test_runtime_retries_identical_frames_then_exhausts);
    RUN_TEST(test_runtime_lost_ack_replays_without_duplicate_commit);
    RUN_TEST(test_runtime_storage_failures_never_produce_transmission_or_ack);
    RUN_TEST(test_runtime_bad_acks_do_not_extend_the_window);
    RUN_TEST(test_runtime_deadline_overrides_queued_valid_ack);
    RUN_TEST(test_runtime_persistence_latency_counts_toward_deadline);
    RUN_TEST(test_runtime_stop_failure_preserves_confirmed_delivery);
    RUN_TEST(test_runtime_time_wrap_and_hardware_completion_timestamp);
    RUN_TEST(test_runtime_pending_operations_have_timeouts);
    RUN_TEST(test_runtime_driver_failures_stop_the_cycle);
    RUN_TEST(test_runtime_sleep_failure_retains_frame_and_blocks_reuse);
    RUN_TEST(test_runtime_random_failure_or_out_of_range_stops);
    RUN_TEST(test_runtime_rejects_invalid_policies_and_samples_before_reserving);
    RUN_TEST(test_runtime_cancel_each_phase_consumes_counter_without_reuse);
    RUN_TEST(test_codec_matches_pre_refactor_wire_fixture);
}
