#include <unity.h>
#include "cajui_protocol.h"
#include <cstring>
#ifdef ARDUINO
#include <Arduino.h>
#endif
using namespace cajui;
void setUp() {}
void tearDown() {}

namespace {
Binding binding() {
    Binding b{}; b.network = 42; b.node = 1234; b.active = true;
    for (size_t i = 0; i < b.key.size(); ++i) b.key[i] = uint8_t(i + 1);
    return b; // Public test vector only, never a production credential.
}
Data sample() {
    Data d{}; d.batteryMv = 3900; d.nextSeconds = 300; d.count = 2;
    d.readings[0].sensor = 1; d.readings[0].metric = 1; d.readings[0].unit = 1;
    d.readings[0].milliValue = -12345;
    d.readings[1].sensor = 1; d.readings[1].metric = 2; d.readings[1].unit = 2;
    d.readings[1].milliValue = 0;
    return d;
}
Frame packet(uint64_t counter = 1) {
    Message m{}; m.counter = counter; m.data = sample(); Frame f{};
    TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(seal(binding(), m, f)));
    return f;
}
void result(Result expected, Result actual) { TEST_ASSERT_EQUAL_INT(int(expected), int(actual)); }
class MemoryJournal : public Journal {
public:
    Receipt state{};
    size_t writes = 0;
    bool readable = true;
    Result writeResult = Result::Ok;
    bool load(const Binding&, Receipt& out) override {
        out = state; return readable;
    }
    Result commit(const Binding&, uint64_t expected, const Receipt& next, const Data&) override {
        if (writeResult != Result::Ok) return writeResult;
        if (expected != state.counter) return Result::Conflict;
        state = next; ++writes; return Result::Ok;
    }
};
class MemoryCounter : public CounterStore {
public:
    uint64_t last = 0;
    bool writable = true;
    bool reserve(const Binding&, uint64_t& out) override {
        if (!writable || last == UINT64_MAX) return false;
        out = ++last; return true;
    }
};
void test_roundtrip_multiple_metrics_and_zero() {
    auto f = packet(); Message m{};
    result(Result::Ok, open(binding(), f, m));
    TEST_ASSERT_EQUAL_UINT64(1, m.counter);
    TEST_ASSERT_EQUAL_UINT16(3900, m.data.batteryMv);
    TEST_ASSERT_EQUAL_UINT32(300, m.data.nextSeconds);
    TEST_ASSERT_EQUAL_UINT8(2, m.data.count);
    TEST_ASSERT_EQUAL_INT32(-12345, m.data.readings[0].milliValue);
    TEST_ASSERT_EQUAL_INT32(0, m.data.readings[1].milliValue);
    TEST_ASSERT_EQUAL_UINT32(75, f.size);
}
void test_all_bytes_are_authenticated() {
    const auto original = packet();
    for (size_t i = 0; i < original.size; ++i) {
        auto modified = original; modified.bytes[i] ^= 1; Message m{};
        TEST_ASSERT_NOT_EQUAL(int(Result::Ok), int(open(binding(), modified, m)));
        TEST_ASSERT_EQUAL_UINT8(0, m.data.count);
    }
}
void test_wrong_key_network_node_and_revocation() {
    auto f = packet(); Message m{}; auto b = binding(); b.key[0] ^= 1;
    result(Result::CryptoError, open(b, f, m));
    b = binding(); ++b.network; result(Result::Unauthorized, open(b, f, m));
    b = binding(); ++b.node; result(Result::Unauthorized, open(b, f, m));
    b = binding(); b.active = false; result(Result::Unauthorized, open(b, f, m));
    m.counter = 1; m.data = sample(); result(Result::Unauthorized, seal(b, m, f));
    TEST_ASSERT_EQUAL_UINT32(0, f.size);
}
void test_lengths_truncation_and_legacy_rejected() {
    auto original = packet(); Message m{};
    for (size_t n = 0; n < original.size; ++n) {
        auto f = original; f.size = n; result(Result::Invalid, open(binding(), f, m));
    }
    auto f = original; ++f.size; result(Result::Invalid, open(binding(), f, m));
    f.size = SIZE_MAX; result(Result::Invalid, open(binding(), f, m));
    f = original; std::memcpy(f.bytes.data(), "CAJUI|3|", 8);
    result(Result::Invalid, open(binding(), f, m));
}
void test_payload_validation_and_extremes() {
    Message m{}; m.counter = UINT64_MAX; m.data = sample(); Frame f{}; Message decoded{};
    m.data.count = MaxReadings;
    for (size_t i = 0; i < MaxReadings; ++i) {
        m.data.readings[i] = sample().readings[0]; m.data.readings[i].sensor = uint16_t(i + 1);
        m.data.readings[i].milliValue = i % 2 ? INT32_MAX : INT32_MIN;
    }
    result(Result::Ok, seal(binding(), m, f)); TEST_ASSERT_EQUAL_UINT32(MaxFrame, f.size);
    result(Result::Ok, open(binding(), f, decoded));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, decoded.data.readings[0].milliValue);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, decoded.data.readings[1].milliValue);
    m.data.count = MaxReadings + 1; result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.count = 0; result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.nextSeconds = 0; result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.readings[1] = m.data.readings[0];
    result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.readings[0].sensor = 0; result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.readings[0].metric = 0; result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.readings[0].unit = 0; result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.data.readings[0].status = Status(3); result(Result::Invalid, seal(binding(), m, f));
    m.data = sample(); m.counter = 0; result(Result::Invalid, seal(binding(), m, f));
    m.counter = 1; m.type = Type(3); result(Result::Invalid, seal(binding(), m, f));
}
void test_sensor_error_is_not_a_measurement() {
    Message m{}; m.counter = 1; m.data = sample(); Frame f{}; Message out{};
    m.data.readings[0].status = Status::Error;
    result(Result::Invalid, seal(binding(), m, f));
    m.data.readings[0].milliValue = 0;
    m.data.readings[1].status = Status::Skipped;
    result(Result::Ok, seal(binding(), m, f)); result(Result::Ok, open(binding(), f, out));
    TEST_ASSERT_EQUAL_UINT8(1, uint8_t(out.data.readings[0].status));
    TEST_ASSERT_EQUAL_UINT8(2, uint8_t(out.data.readings[1].status));
}
void test_lost_ack_retries_without_duplicate_storage() {
    MemoryCounter counter; MemoryJournal journal; Sender tx; Frame ack{};
    result(Result::Ok, tx.begin(binding(), sample(), counter));
    const auto first = *tx.nextAttempt();
    result(Result::Ok, receive(binding(), first, journal, ack));
    // Lost ACK: retry the same DATA with the previously committed receipt.
    const auto retry = *tx.nextAttempt(); TEST_ASSERT_TRUE(sameFrame(first, retry));
    Frame repeatedAck{}; result(Result::Duplicate, receive(binding(), retry, journal, repeatedAck));
    TEST_ASSERT_EQUAL_UINT32(1, journal.writes);
    TEST_ASSERT_TRUE(sameFrame(ack, repeatedAck));
    result(Result::Ok, tx.acknowledge(repeatedAck)); TEST_ASSERT_TRUE(tx.delivered());
    TEST_ASSERT_NULL(tx.nextAttempt());
}
void test_no_ack_before_durable_commit() {
    auto f = packet(); Frame ack{}; MemoryJournal journal;
    journal.readable = false; result(Result::StorageError, receive(binding(), f, journal, ack));
    TEST_ASSERT_EQUAL_UINT32(0, ack.size);
    journal.readable = true;
    for (auto failure : {Result::Full, Result::StorageError, Result::Conflict}) {
        journal.writeResult = failure; ack = f;
        result(failure, receive(binding(), f, journal, ack));
        TEST_ASSERT_EQUAL_UINT32(0, ack.size); TEST_ASSERT_EQUAL_UINT64(0, journal.state.counter);
    }
    journal.writeResult = Result::Ok; result(Result::Ok, receive(binding(), f, journal, ack));
    journal.writeResult = Result::Full;
    result(Result::Duplicate, receive(binding(), f, journal, ack));
    TEST_ASSERT_GREATER_THAN_UINT32(0, ack.size);
}
void test_replay_conflict_and_invalid_input_do_not_commit() {
    Frame ack{}; MemoryJournal journal;
    result(Result::Ok, receive(binding(), packet(2), journal, ack));
    result(Result::Replay, receive(binding(), packet(1), journal, ack));
    Message conflict{}; conflict.counter = 2; conflict.data = sample();
    conflict.data.batteryMv = 4000; Frame changed{};
    // Deliberately misuse a counter to simulate a faulty sender.
    result(Result::Ok, seal(binding(), conflict, changed));
    result(Result::Conflict, receive(binding(), changed, journal, ack));
    changed.bytes[50] ^= 1; result(Result::CryptoError, receive(binding(), changed, journal, ack));
    TEST_ASSERT_EQUAL_UINT32(1, journal.writes); TEST_ASSERT_EQUAL_UINT32(0, ack.size);
}
void test_sender_limits_and_counter_survives_restart() {
    MemoryCounter counter; Sender tx; TEST_ASSERT_NULL(tx.nextAttempt());
    Frame empty{}; result(Result::Invalid, tx.acknowledge(empty));
    result(Result::Ok, tx.begin(binding(), sample(), counter));
    result(Result::Conflict, tx.begin(binding(), sample(), counter));
    for (int i = 0; i < 3; ++i) TEST_ASSERT_NOT_NULL(tx.nextAttempt());
    TEST_ASSERT_NULL(tx.nextAttempt()); TEST_ASSERT_FALSE(tx.delivered());
    result(Result::Conflict, tx.begin(binding(), sample(), counter));
    tx.abandon();
    result(Result::Ok, tx.begin(binding(), sample(), counter));
    Sender restarted; result(Result::Ok, restarted.begin(binding(), sample(), counter));
    Message m{}; result(Result::Ok, open(binding(), *restarted.nextAttempt(), m));
    TEST_ASSERT_EQUAL_UINT64(3, m.counter);
    Sender blocked; counter.writable = false;
    result(Result::StorageError, blocked.begin(binding(), sample(), counter));
    TEST_ASSERT_NULL(blocked.nextAttempt());
    counter.writable = true; counter.last = UINT64_MAX;
    result(Result::StorageError, blocked.begin(binding(), sample(), counter));
    auto b = binding(); b.active = false;
    result(Result::Unauthorized, blocked.begin(b, sample(), counter));
    auto d = sample(); d.count = 0; result(Result::Invalid, blocked.begin(binding(), d, counter));
}
void test_ack_must_match_pending_data() {
    Sender tx; MemoryCounter counter; MemoryJournal journal; Frame ack{};
    result(Result::Ok, tx.begin(binding(), sample(), counter));
    result(Result::Invalid, tx.acknowledge(packet()));
    const auto data = *tx.nextAttempt();
    result(Result::Invalid, tx.acknowledge(data));
    result(Result::Ok, receive(binding(), packet(2), journal, ack));
    result(Result::Invalid, tx.acknowledge(ack));
    Message wrong{}; wrong.type = Type::Ack; wrong.counter = 1;
    result(Result::Ok, seal(binding(), wrong, ack));
    result(Result::Invalid, tx.acknowledge(ack));
    ack.bytes[ack.size - 1] ^= 1; result(Result::CryptoError, tx.acknowledge(ack));
    TEST_ASSERT_FALSE(tx.delivered());
    Frame ignored{}; result(Result::CryptoError, receive(binding(), ack, journal, ignored));
    result(Result::Ok, seal(binding(), wrong, ack));
    result(Result::Invalid, receive(binding(), ack, journal, ignored));
}
void test_authenticated_malformed_payload_is_rejected() {
    const auto original = packet();
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto f = original;
        uint8_t nonce[12] = {'C', 'J', 1, 1, 0, 0, 0, 0, 0, 0, 0, 1};
        uint8_t plain[MaxPayload]{};
        const size_t size = f.size - HeaderSize - TagSize;
        TEST_ASSERT_TRUE(decrypt(binding().key, nonce, f.bytes.data(), HeaderSize,
            f.bytes.data() + HeaderSize, size, f.bytes.data() + f.size - TagSize, plain));
        switch (scenario) {
        case 0: plain[6] = 255; break; // count exceeds capacity
        case 1: plain[6] = 1; break; // count disagrees with length
        case 2: plain[12] = 3; break; // unknown status
        case 3: plain[7] = plain[8] = 0; break; // zero sensor ID
        default: plain[12] = 1; break; // error carrying a measurement
        }
        TEST_ASSERT_TRUE(encrypt(binding().key, nonce, f.bytes.data(), HeaderSize,
            plain, size, f.bytes.data() + HeaderSize, f.bytes.data() + f.size - TagSize));
        Message out{}; result(Result::Invalid, open(binding(), f, out));
        TEST_ASSERT_EQUAL_UINT8(0, out.data.count);
    }
}
void test_untrusted_lengths_and_bytes_under_sanitizers() {
    uint32_t random = 17;
    for (size_t trial = 0; trial < 2000; ++trial) {
        Frame f{};
        for (auto& byte : f.bytes) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            byte = uint8_t(random);
        }
        f.size = trial % (MaxFrame + 20); Message out{};
        TEST_ASSERT_NOT_EQUAL(int(Result::Ok), int(open(binding(), f, out)));
    }
}
void test_wire_header_is_portable_and_ack_direction_is_separate() {
    auto f = packet();
    const uint8_t expected[32] = {'C','J','L','R',1,1,
        0,0,0,0,0,0,0,42, 0,0,0,0,0,0,4,210, 0,0,0,0,0,0,0,1, 0,27};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, f.bytes.data(), 32);
    Message ack{}; ack.type = Type::Ack; ack.counter = 1;
    Frame response{}; result(Result::Ok, seal(binding(), ack, response));
    // Changing direction does not create an authenticated message of another type.
    response.bytes[5] = 1;
    Message out{}; TEST_ASSERT_NOT_EQUAL(int(Result::Ok), int(open(binding(), response, out)));
    response.size = MaxFrame + 1; TEST_ASSERT_FALSE(sameFrame(response, response));
}
void test_nist_aes_gcm_known_answer_and_failure_wipes_output() {
    // NIST GCM: AES-128, zero key/IV/plaintext, no AAD, one 16-byte block.
    const uint8_t expectedCipher[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,
        0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
    const uint8_t expectedTag[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
        0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
    Key key{}; uint8_t nonce[12]{}, plain[16]{}, cipher[16]{}, tag[16]{}, out[16]{};
    TEST_ASSERT_TRUE(encrypt(key, nonce, plain, 0, plain, 16, cipher, tag));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedCipher, cipher, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedTag, tag, 16);
    TEST_ASSERT_TRUE(decrypt(key, nonce, plain, 0, cipher, 16, tag, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(plain, out, 16);
    tag[0] ^= 1; std::memset(out, 0xff, 16);
    TEST_ASSERT_FALSE(decrypt(key, nonce, plain, 0, cipher, 16, tag, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(plain, out, 16);
}
}
void runRuntimeTests();
void runStorageTests();
void runProvisioningTests();
int runTests() {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_multiple_metrics_and_zero);
    RUN_TEST(test_all_bytes_are_authenticated);
    RUN_TEST(test_wrong_key_network_node_and_revocation);
    RUN_TEST(test_lengths_truncation_and_legacy_rejected);
    RUN_TEST(test_payload_validation_and_extremes);
    RUN_TEST(test_sensor_error_is_not_a_measurement);
    RUN_TEST(test_lost_ack_retries_without_duplicate_storage);
    RUN_TEST(test_no_ack_before_durable_commit);
    RUN_TEST(test_replay_conflict_and_invalid_input_do_not_commit);
    RUN_TEST(test_sender_limits_and_counter_survives_restart);
    RUN_TEST(test_ack_must_match_pending_data);
    RUN_TEST(test_authenticated_malformed_payload_is_rejected);
    RUN_TEST(test_untrusted_lengths_and_bytes_under_sanitizers);
    RUN_TEST(test_wire_header_is_portable_and_ack_direction_is_separate);
    RUN_TEST(test_nist_aes_gcm_known_answer_and_failure_wipes_output);
    runRuntimeTests();
    runStorageTests();
    runProvisioningTests();
    return UNITY_END();
}
#ifdef ARDUINO
void setup() { delay(2000); runTests(); }
void loop() {}
#else
int main() { return runTests(); }
#endif
