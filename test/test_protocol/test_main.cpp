// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include "cajui_protocol.h"
#include "assertions.h"
#include "storage_support.h"
#include <cstring>
#ifdef ARDUINO
#include <Arduino.h>
#endif
using namespace cajui;
void setUp() {}
void tearDown() {}

namespace {
Binding vectorBinding() {
    Binding b{};
    b.network = 42;
    b.node = 1234;
    b.active = true;
    for (size_t i = 0; i < b.key.size(); ++i) b.key[i] = uint8_t(i + 1);
    return b; // Public test vector only, never a production credential.
}
Data twoReadings() {
    Data d{};
    d.batteryMv = 3900;
    d.nextSeconds = 300;
    d.count = 2;
    d.readings[0].sensor = 1;
    d.readings[0].metric = 1;
    d.readings[0].unit = 1;
    d.readings[0].milliValue = -12345;
    d.readings[1].sensor = 1;
    d.readings[1].metric = 2;
    d.readings[1].unit = 2;
    d.readings[1].milliValue = 0;
    return d;
}
Frame packet(uint64_t counter = 1) {
    Message m{};
    m.counter = counter;
    m.data = twoReadings();
    Frame f{};
    TEST_ASSERT_EQUAL_INT(int(Result::Ok), int(seal(vectorBinding(), m, f)));
    return f;
}
class MemoryJournal : public Journal {
public:
    Receipt state{};
    size_t writes = 0;
    bool readable = true;
    Result writeResult = Result::Ok;
    bool load(const Binding&, Receipt& out) override {
        out = state;
        return readable;
    }
    Result commit(const Binding&, uint64_t expected, const Receipt& next) override {
        if (writeResult != Result::Ok) return writeResult;
        if (expected != state.counter) return Result::Conflict;
        state = next;
        ++writes;
        return Result::Ok;
    }
};
class MemoryCounter : public CounterStore {
public:
    uint64_t last = 0;
    bool writable = true;
    bool reserve(const Binding&, uint64_t& out) override {
        if (!writable || last == UINT64_MAX) return false;
        out = ++last;
        return true;
    }
};
void test_roundtrip_multiple_metrics_and_zero() {
    auto f = packet();
    Message m{};
    EXPECT_RESULT(Result::Ok, open(vectorBinding(), f, m));
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
        SCENARIO(i);
        auto modified = original;
        modified.bytes[i] ^= 1;
        Message m{};
        TEST_ASSERT_NOT_EQUAL(int(Result::Ok), int(open(vectorBinding(), modified, m)));
        TEST_ASSERT_EQUAL_UINT8(0, m.data.count);
    }
}
void test_wrong_key_network_node_and_revocation() {
    auto f = packet();
    Message m{};
    auto b = vectorBinding();
    b.key[0] ^= 1;
    EXPECT_RESULT(Result::CryptoError, open(b, f, m));
    b = vectorBinding();
    ++b.network;
    EXPECT_RESULT(Result::Unauthorized, open(b, f, m));
    b = vectorBinding();
    ++b.node;
    EXPECT_RESULT(Result::Unauthorized, open(b, f, m));
    b = vectorBinding();
    b.active = false;
    EXPECT_RESULT(Result::Unauthorized, open(b, f, m));
    m.counter = 1;
    m.data = twoReadings();
    EXPECT_RESULT(Result::Unauthorized, seal(b, m, f));
    TEST_ASSERT_EQUAL_UINT32(0, f.size);
}
void test_lengths_truncation_and_legacy_rejected() {
    auto original = packet();
    Message m{};
    for (size_t n = 0; n < original.size; ++n) {
        SCENARIO(n);
        auto f = original;
        f.size = n;
        EXPECT_RESULT(Result::Invalid, open(vectorBinding(), f, m));
    }
    auto f = original;
    ++f.size;
    EXPECT_RESULT(Result::Invalid, open(vectorBinding(), f, m));
    f.size = SIZE_MAX;
    EXPECT_RESULT(Result::Invalid, open(vectorBinding(), f, m));
    f = original;
    std::memcpy(f.bytes.data(), "CAJUI|3|", 8);
    EXPECT_RESULT(Result::Invalid, open(vectorBinding(), f, m));
}
void test_payload_validation_and_extremes() {
    Message m{};
    m.counter = UINT64_MAX;
    m.data = twoReadings();
    Frame f{};
    Message decoded{};
    m.data.count = MaxReadings;
    for (size_t i = 0; i < MaxReadings; ++i) {
        m.data.readings[i] = twoReadings().readings[0];
        m.data.readings[i].sensor = uint16_t(i + 1);
        m.data.readings[i].milliValue = i % 2 ? INT32_MAX : INT32_MIN;
    }
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), m, f));
    TEST_ASSERT_EQUAL_UINT32(MaxFrame, f.size);
    EXPECT_RESULT(Result::Ok, open(vectorBinding(), f, decoded));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, decoded.data.readings[0].milliValue);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, decoded.data.readings[1].milliValue);
    m.data.count = MaxReadings + 1;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.count = 0;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.nextSeconds = 0;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.readings[1] = m.data.readings[0];
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.readings[0].sensor = 0;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.readings[0].metric = 0;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.readings[0].unit = 0;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.data.readings[0].status = Status(3);
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data = twoReadings();
    m.counter = 0;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.counter = 1;
    m.type = Type(3);
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
}
void test_sensor_error_is_not_a_measurement() {
    Message m{};
    m.counter = 1;
    m.data = twoReadings();
    Frame f{};
    Message out{};
    m.data.readings[0].status = Status::Error;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), m, f));
    m.data.readings[0].milliValue = 0;
    m.data.readings[1].status = Status::Skipped;
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), m, f));
    EXPECT_RESULT(Result::Ok, open(vectorBinding(), f, out));
    TEST_ASSERT_EQUAL_UINT8(1, uint8_t(out.data.readings[0].status));
    TEST_ASSERT_EQUAL_UINT8(2, uint8_t(out.data.readings[1].status));
}
void test_lost_ack_retries_without_duplicate_storage() {
    MemoryCounter counter;
    MemoryJournal journal;
    Sender tx;
    Frame ack{};
    EXPECT_RESULT(Result::Ok, tx.begin(vectorBinding(), twoReadings(), counter));
    const auto first = *tx.nextAttempt();
    EXPECT_RESULT(Result::Ok, receive(vectorBinding(), first, journal, ack));
    // Lost ACK: retry the same DATA with the previously committed receipt.
    const auto retry = *tx.nextAttempt();
    TEST_ASSERT_TRUE(sameFrame(first, retry));
    Frame repeatedAck{};
    EXPECT_RESULT(Result::Duplicate, receive(vectorBinding(), retry, journal, repeatedAck));
    TEST_ASSERT_EQUAL_UINT32(1, journal.writes);
    TEST_ASSERT_TRUE(sameFrame(ack, repeatedAck));
    EXPECT_RESULT(Result::Ok, tx.acknowledge(repeatedAck));
    TEST_ASSERT_TRUE(tx.delivered());
    TEST_ASSERT_NULL(tx.nextAttempt());
}
void test_no_ack_before_durable_commit() {
    auto f = packet();
    Frame ack{};
    MemoryJournal journal;
    journal.readable = false;
    EXPECT_RESULT(Result::StorageError, receive(vectorBinding(), f, journal, ack));
    TEST_ASSERT_EQUAL_UINT32(0, ack.size);
    journal.readable = true;
    for (auto failure : {Result::Full, Result::StorageError, Result::Conflict}) {
        SCENARIO(int(failure));
        journal.writeResult = failure;
        ack = f;
        EXPECT_RESULT(failure, receive(vectorBinding(), f, journal, ack));
        TEST_ASSERT_EQUAL_UINT32(0, ack.size);
        TEST_ASSERT_EQUAL_UINT64(0, journal.state.counter);
    }
    journal.writeResult = Result::Ok;
    EXPECT_RESULT(Result::Ok, receive(vectorBinding(), f, journal, ack));
    journal.writeResult = Result::Full;
    EXPECT_RESULT(Result::Duplicate, receive(vectorBinding(), f, journal, ack));
    TEST_ASSERT_GREATER_THAN_UINT32(0, ack.size);
}
void test_replay_conflict_and_invalid_input_do_not_commit() {
    Frame ack{};
    MemoryJournal journal;
    EXPECT_RESULT(Result::Ok, receive(vectorBinding(), packet(2), journal, ack));
    EXPECT_RESULT(Result::Replay, receive(vectorBinding(), packet(1), journal, ack));
    Message conflict{};
    conflict.counter = 2;
    conflict.data = twoReadings();
    conflict.data.batteryMv = 4000;
    Frame changed{};
    // Deliberately misuse a counter to simulate a faulty sender.
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), conflict, changed));
    EXPECT_RESULT(Result::Conflict, receive(vectorBinding(), changed, journal, ack));
    changed.bytes[50] ^= 1;
    EXPECT_RESULT(Result::CryptoError, receive(vectorBinding(), changed, journal, ack));
    TEST_ASSERT_EQUAL_UINT32(1, journal.writes);
    TEST_ASSERT_EQUAL_UINT32(0, ack.size);
}
void test_sender_limits_and_counter_survives_restart() {
    MemoryCounter counter;
    Sender tx;
    TEST_ASSERT_NULL(tx.nextAttempt());
    Frame empty{};
    EXPECT_RESULT(Result::Invalid, tx.acknowledge(empty));
    EXPECT_RESULT(Result::Ok, tx.begin(vectorBinding(), twoReadings(), counter));
    EXPECT_RESULT(Result::Conflict, tx.begin(vectorBinding(), twoReadings(), counter));
    for (int i = 0; i < 3; ++i) TEST_ASSERT_NOT_NULL(tx.nextAttempt());
    TEST_ASSERT_NULL(tx.nextAttempt());
    TEST_ASSERT_FALSE(tx.delivered());
    EXPECT_RESULT(Result::Conflict, tx.begin(vectorBinding(), twoReadings(), counter));
    tx.abandon();
    EXPECT_RESULT(Result::Ok, tx.begin(vectorBinding(), twoReadings(), counter));
    Sender restarted;
    EXPECT_RESULT(Result::Ok, restarted.begin(vectorBinding(), twoReadings(), counter));
    Message m{};
    EXPECT_RESULT(Result::Ok, open(vectorBinding(), *restarted.nextAttempt(), m));
    TEST_ASSERT_EQUAL_UINT64(3, m.counter);
    Sender blocked;
    counter.writable = false;
    EXPECT_RESULT(Result::StorageError, blocked.begin(vectorBinding(), twoReadings(), counter));
    TEST_ASSERT_NULL(blocked.nextAttempt());
    counter.writable = true;
    counter.last = UINT64_MAX;
    EXPECT_RESULT(Result::StorageError, blocked.begin(vectorBinding(), twoReadings(), counter));
    auto b = vectorBinding();
    b.active = false;
    EXPECT_RESULT(Result::Unauthorized, blocked.begin(b, twoReadings(), counter));
    auto d = twoReadings();
    d.count = 0;
    EXPECT_RESULT(Result::Invalid, blocked.begin(vectorBinding(), d, counter));
}
void test_ack_must_match_pending_data() {
    Sender tx;
    MemoryCounter counter;
    MemoryJournal journal;
    Frame ack{};
    EXPECT_RESULT(Result::Ok, tx.begin(vectorBinding(), twoReadings(), counter));
    EXPECT_RESULT(Result::Invalid, tx.acknowledge(packet()));
    const auto data = *tx.nextAttempt();
    EXPECT_RESULT(Result::Invalid, tx.acknowledge(data));
    EXPECT_RESULT(Result::Ok, receive(vectorBinding(), packet(2), journal, ack));
    EXPECT_RESULT(Result::Invalid, tx.acknowledge(ack));
    Message wrong{};
    wrong.type = Type::Ack;
    wrong.counter = 1;
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), wrong, ack));
    EXPECT_RESULT(Result::Invalid, tx.acknowledge(ack));
    ack.bytes[ack.size - 1] ^= 1;
    EXPECT_RESULT(Result::CryptoError, tx.acknowledge(ack));
    TEST_ASSERT_FALSE(tx.delivered());
    Frame ignored{};
    EXPECT_RESULT(Result::CryptoError, receive(vectorBinding(), ack, journal, ignored));
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), wrong, ack));
    EXPECT_RESULT(Result::Invalid, receive(vectorBinding(), ack, journal, ignored));
}
void test_authenticated_malformed_payload_is_rejected() {
    const auto original = packet();
    for (int scenario = 0; scenario < 5; ++scenario) {
        SCENARIO(scenario);
        auto f = original;
        uint8_t nonce[12] = {'C', 'J', 1, 1, 0, 0, 0, 0, 0, 0, 0, 1};
        uint8_t plain[MaxPayload]{};
        const size_t size = f.size - HeaderSize - TagSize;
        TEST_ASSERT_TRUE(decrypt(vectorBinding().key, nonce, f.bytes.data(), HeaderSize,
                                 f.bytes.data() + HeaderSize, size,
                                 f.bytes.data() + f.size - TagSize, plain));
        switch (scenario) {
        case 0: plain[6] = 255; break;          // count exceeds capacity
        case 1: plain[6] = 1; break;            // count disagrees with length
        case 2: plain[12] = 3; break;           // unknown status
        case 3: plain[7] = plain[8] = 0; break; // zero sensor ID
        default: plain[12] = 1; break;          // error carrying a measurement
        }
        TEST_ASSERT_TRUE(encrypt(vectorBinding().key, nonce, f.bytes.data(), HeaderSize, plain,
                                 size, f.bytes.data() + HeaderSize,
                                 f.bytes.data() + f.size - TagSize));
        Message out{};
        EXPECT_RESULT(Result::Invalid, open(vectorBinding(), f, out));
        TEST_ASSERT_EQUAL_UINT8(0, out.data.count);
    }
}
void test_untrusted_lengths_and_bytes_under_sanitizers() {
    uint32_t random = 17;
    for (size_t trial = 0; trial < 2000; ++trial) {
        SCENARIO(trial);
        Frame f{};
        for (auto& byte : f.bytes) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            byte = uint8_t(random);
        }
        f.size = trial % (MaxFrame + 20);
        Message out{};
        TEST_ASSERT_NOT_EQUAL(int(Result::Ok), int(open(vectorBinding(), f, out)));
    }
}
void test_wire_header_is_portable_and_ack_direction_is_separate() {
    auto f = packet();
    const uint8_t expected[32] = {'C', 'J', 'L', 'R', 1, 1,   0, 0, 0, 0, 0, 0, 0, 42, 0, 0,
                                  0,   0,   0,   0,   4, 210, 0, 0, 0, 0, 0, 0, 0, 1,  0, 27};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, f.bytes.data(), 32);
    Message ack{};
    ack.type = Type::Ack;
    ack.counter = 1;
    Frame response{};
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), ack, response));
    // Changing direction does not create an authenticated message of another type.
    response.bytes[5] = 1;
    Message out{};
    TEST_ASSERT_NOT_EQUAL(int(Result::Ok), int(open(vectorBinding(), response, out)));
    response.size = MaxFrame + 1;
    TEST_ASSERT_FALSE(sameFrame(response, response));
}
void test_codec_matches_pre_refactor_wire_fixture() {
    const uint8_t expected[] = {0x43, 0x4a, 0x4c, 0x52, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11, 0xe3,
                                0x30, 0xa6, 0x91, 0xf0, 0x79, 0x7a, 0xcd, 0xcf, 0xac, 0x2b, 0xc4,
                                0x6c, 0xa5, 0xed, 0xcf, 0xeb, 0x02, 0x90, 0x30, 0x9f, 0xc1, 0xec,
                                0x18, 0xdf, 0xca, 0xd0, 0x5f, 0x45, 0x3e, 0xc9, 0x87, 0x9a};
    auto frame = fixtures::data(1);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), frame.size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame.bytes.data(), sizeof(expected));
}
void test_v2_ack_carries_an_authenticated_power_command_and_v1_still_works() {
    for (uint8_t version : {WireV1, WireV2}) {
        SCENARIO(version);
        MemoryJournal journal;
        MemoryCounter counter;
        Sender sender;
        EXPECT_RESULT(Result::Ok, sender.begin(vectorBinding(), twoReadings(), counter, version));
        const Frame data = *sender.nextAttempt();
        TEST_ASSERT_EQUAL_UINT8(version, data.bytes[4]);
        TEST_ASSERT_EQUAL_UINT64(1234, untrustedDataNode(data));
        Frame ack{};
        Link link{};
        link.known = true;
        link.rssiDbm = -71;
        link.snrTenthsDb = 95;
        EXPECT_RESULT(Result::Ok, receive(vectorBinding(), data, journal, ack, link, 14));
        TEST_ASSERT_TRUE(journal.state.link.known);
        TEST_ASSERT_EQUAL_INT16(-71, journal.state.link.rssiDbm);
        Message decoded{};
        EXPECT_RESULT(Result::Ok, open(vectorBinding(), ack, decoded));
        TEST_ASSERT_EQUAL_UINT8(version, decoded.version);
        // A v1 node never receives a command it cannot parse.
        TEST_ASSERT_EQUAL_INT8(version == WireV2 ? 14 : KeepPower, decoded.powerDbm);
        TEST_ASSERT_EQUAL_size_t(HeaderSize + TagSize + (version == WireV2 ? 1 : 0) + TagSize,
                                 ack.size);
        Frame tampered = ack;
        tampered.bytes[HeaderSize + TagSize] ^= 1; // The command is authenticated.
        EXPECT_RESULT(Result::CryptoError, sender.acknowledge(tampered));
        EXPECT_RESULT(Result::Ok, sender.acknowledge(ack));
        TEST_ASSERT_EQUAL_INT8(version == WireV2 ? 14 : KeepPower, sender.powerCommand());
    }
    // An ACK in another version than the DATA is refused, and v1 cannot carry a command.
    MemoryCounter counter;
    Sender sender;
    EXPECT_RESULT(Result::Ok, sender.begin(vectorBinding(), twoReadings(), counter));
    const Frame data = *sender.nextAttempt();
    Message ack{};
    ack.type = Type::Ack;
    ack.version = WireV1;
    ack.counter = 1;
    std::memcpy(ack.dataTag.data(), data.bytes.data() + data.size - TagSize, TagSize);
    Frame frame{};
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), ack, frame));
    EXPECT_RESULT(Result::Invalid, sender.acknowledge(frame));
    TEST_ASSERT_EQUAL_INT8(KeepPower, sender.powerCommand());
    ack.powerDbm = 10;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), ack, frame));
    ack.version = 3;
    EXPECT_RESULT(Result::Invalid, seal(vectorBinding(), ack, frame));
    Sender other;
    EXPECT_RESULT(Result::Invalid, other.begin(vectorBinding(), twoReadings(), counter, 3));
    // The version is part of the nonce: the same counter in v1 and v2 never shares one.
    Message m{};
    m.counter = 9;
    m.data = twoReadings();
    Frame v1{}, v2{};
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), m, v1));
    m.version = WireV2;
    EXPECT_RESULT(Result::Ok, seal(vectorBinding(), m, v2));
    TEST_ASSERT_EQUAL_size_t(v1.size, v2.size);
    TEST_ASSERT_FALSE(std::memcmp(v1.bytes.data() + HeaderSize, v2.bytes.data() + HeaderSize,
                                  v1.size - HeaderSize) == 0);
    v2.bytes[4] = 3; // Unknown versions are not even routed.
    TEST_ASSERT_EQUAL_UINT64(0, untrustedDataNode(v2));
    TEST_ASSERT_EQUAL_UINT8(0, untrustedType(v2));
}
void test_nist_aes_gcm_known_answer_and_failure_wipes_output() {
    // NIST GCM: AES-128, zero key/IV/plaintext, no AAD, one 16-byte block.
    const uint8_t expectedCipher[16] = {0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
                                        0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78};
    const uint8_t expectedTag[16] = {0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd,
                                     0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf};
    Key key{};
    uint8_t nonce[12]{}, plain[16]{}, cipher[16]{}, tag[16]{}, out[16]{};
    TEST_ASSERT_TRUE(encrypt(key, nonce, plain, 0, plain, 16, cipher, tag));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedCipher, cipher, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedTag, tag, 16);
    TEST_ASSERT_TRUE(decrypt(key, nonce, plain, 0, cipher, 16, tag, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(plain, out, 16);
    tag[0] ^= 1;
    std::memset(out, 0xff, 16);
    TEST_ASSERT_FALSE(decrypt(key, nonce, plain, 0, cipher, 16, tag, out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(plain, out, 16);
    // Sizes beyond the protocol bounds are refused before reaching the library.
    static uint8_t big[MaxFrame + 1];
    TEST_ASSERT_FALSE(encrypt(key, nonce, big, MaxFrame + 1, plain, 16, cipher, tag));
    TEST_ASSERT_FALSE(encrypt(key, nonce, plain, 0, big, MaxPayload + 1, big, tag));
    TEST_ASSERT_FALSE(decrypt(key, nonce, big, MaxFrame + 1, cipher, 16, tag, out));
    TEST_ASSERT_FALSE(decrypt(key, nonce, plain, 0, big, MaxPayload + 1, tag, big));
}
}
void runApplicationTests();
void runRuntimeTests();
void runStorageTests();
void runProvisioningTests();
void runUplinkTests();
void runSetupTests();
void runPairingTests();
void runDeviceTests();
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
    RUN_TEST(test_v2_ack_carries_an_authenticated_power_command_and_v1_still_works);
    RUN_TEST(test_codec_matches_pre_refactor_wire_fixture);
    runApplicationTests();
    runRuntimeTests();
    runStorageTests();
    runProvisioningTests();
    runUplinkTests();
    runSetupTests();
    runPairingTests();
    runDeviceTests();
    return UNITY_END();
}
#ifdef ARDUINO
void setup() {
    delay(2000);
    runTests();
}
void loop() {}
#else
int main() {
    return runTests();
}
#endif
