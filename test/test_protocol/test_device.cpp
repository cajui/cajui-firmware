// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include "assertions.h"
#include "cajui_device.h"
#include "storage_support.h"

namespace {
using namespace cajui;
using namespace fixtures;
constexpr uint16_t Profile = 1;

BootDecision boot(PersistentStore& store, bool mounted, BootRequest request, bool held = false) {
    return decideBoot(store, mounted, Profile, request, held);
}
#define EXPECT_BOOT(expectedMode, expectedReason, decision)                                        \
    do {                                                                                           \
        const BootDecision d = (decision);                                                         \
        EXPECT_RESULT(expectedMode, d.mode);                                                       \
        EXPECT_RESULT(expectedReason, d.reason);                                                   \
    } while (0)

void test_transmitter_boot_modes() {
    MemoryRecords records;
    auto store = mounted(records);
    // Unenrolled: admin, unless pairing was asked for by button or console.
    EXPECT_BOOT(BootMode::Admin, AdminReason::NotEnrolled, boot(*store, true, BootRequest::None));
    EXPECT_BOOT(BootMode::Pair, AdminReason::None, boot(*store, true, BootRequest::None, true));
    EXPECT_BOOT(BootMode::Pair, AdminReason::None, boot(*store, true, BootRequest::Pair));
    // An admin request wins over everything, including a held button.
    EXPECT_BOOT(BootMode::Admin, AdminReason::Requested,
                boot(*store, true, BootRequest::Admin, true));
    // Unusable storage never pairs or runs.
    EXPECT_BOOT(BootMode::Admin, AdminReason::Storage, boot(*store, false, BootRequest::Pair));
    TEST_ASSERT_TRUE(enroll(*store));
    EXPECT_BOOT(BootMode::Run, AdminReason::None, boot(*store, true, BootRequest::None));
    EXPECT_BOOT(BootMode::Pair, AdminReason::None, boot(*store, true, BootRequest::None, true));
    // Revoked locally: no active binding of its own.
    EXPECT_RESULT(Result::Ok, store->revoke(2, 10));
    EXPECT_BOOT(BootMode::Admin, AdminReason::NotEnrolled, boot(*store, true, BootRequest::None));
    // Another radio profile is not runnable.
    TEST_ASSERT_TRUE(enroll(*store, 2, 11, 2));
    EXPECT_BOOT(BootMode::Admin, AdminReason::NotEnrolled,
                decideBoot(*store, true, 2, BootRequest::None, false));
}
void test_receiver_boot_modes() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    // A receiver without bindings runs, so radio pairing can create the first one.
    EXPECT_BOOT(BootMode::Run, AdminReason::None, boot(*store, true, BootRequest::None));
    EXPECT_BOOT(BootMode::Run, AdminReason::None, boot(*store, true, BootRequest::Pair, true));
    EXPECT_BOOT(BootMode::Admin, AdminReason::Requested, boot(*store, true, BootRequest::Admin));
    EXPECT_BOOT(BootMode::Admin, AdminReason::Storage, boot(*store, false, BootRequest::None));
    TEST_ASSERT_TRUE(enroll(*store));
    EXPECT_BOOT(BootMode::Run, AdminReason::None, boot(*store, true, BootRequest::None));
    EXPECT_BOOT(BootMode::Admin, AdminReason::NotEnrolled,
                decideBoot(*store, true, 2, BootRequest::None, false));
    TEST_ASSERT_EQUAL_STRING("requested", reasonName(AdminReason::Requested));
    TEST_ASSERT_EQUAL_STRING("storage", reasonName(AdminReason::Storage));
    TEST_ASSERT_EQUAL_STRING("not_enrolled", reasonName(AdminReason::NotEnrolled));
    TEST_ASSERT_EQUAL_STRING("none", reasonName(AdminReason::None));
}
void test_fault_retry_delay_doubles_up_to_a_bound() {
    TEST_ASSERT_EQUAL_UINT32(FirstRetryMs, retryDelayMs(0));
    TEST_ASSERT_EQUAL_UINT32(FirstRetryMs, retryDelayMs(1));
    TEST_ASSERT_EQUAL_UINT32(2 * FirstRetryMs, retryDelayMs(2));
    TEST_ASSERT_EQUAL_UINT32(8 * FirstRetryMs, retryDelayMs(4));
    TEST_ASSERT_EQUAL_UINT32(MaxRetryMs, retryDelayMs(8));
    TEST_ASSERT_EQUAL_UINT32(MaxRetryMs, retryDelayMs(UINT32_MAX)); // No overflow, bounded loop.
    uint32_t previous = 0;
    for (uint32_t faults = 1; faults < 40; ++faults) {
        const uint32_t delay = retryDelayMs(faults);
        TEST_ASSERT_TRUE(delay >= previous && delay <= MaxRetryMs);
        previous = delay;
    }
}
void test_power_record_round_trips_and_fails_closed() {
    MemoryBlob blob;
    int8_t dbm = 5;
    EXPECT_RESULT(ReadResult::Missing, loadPower(blob, dbm));
    TEST_ASSERT_EQUAL_INT8(DefaultPowerDbm, dbm);
    TEST_ASSERT_FALSE(savePower(blob, 23));
    TEST_ASSERT_FALSE(savePower(blob, -10));
    TEST_ASSERT_TRUE(savePower(blob, -3));
    EXPECT_RESULT(ReadResult::Ok, loadPower(blob, dbm));
    TEST_ASSERT_EQUAL_INT8(-3, dbm);
    for (size_t i = 0; i < RadioRecordSize; ++i) {
        SCENARIO(i);
        MemoryBlob damaged = blob;
        damaged.bytes[i] ^= 0x40;
        EXPECT_RESULT(ReadResult::Error, loadPower(damaged, dbm));
        TEST_ASSERT_EQUAL_INT8(DefaultPowerDbm, dbm);
    }
    MemoryBlob outOfRange = blob;
    outOfRange.bytes[2] = 30; // Valid CRC, unsupported value.
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < 3; ++i) {
        crc ^= outOfRange.bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    crc = ~crc;
    for (int i = 0; i < 4; ++i) outOfRange.bytes[3 + i] = uint8_t(crc >> (8 * (3 - i)));
    EXPECT_RESULT(ReadResult::Error, loadPower(outOfRange, dbm));
    blob.size = RadioRecordSize - 1;
    EXPECT_RESULT(ReadResult::Error, loadPower(blob, dbm));
}
void test_power_follows_commands_within_the_ceiling_and_falls_back() {
    // Without a remembered state the node uses the configured power.
    TEST_ASSERT_EQUAL_INT8(10, currentPower(nullptr, 10));
    TEST_ASSERT_EQUAL_INT8(DefaultPowerDbm, currentPower(nullptr, 99)); // Invalid ceiling.
    PowerState state{};
    state.ceiling = 10;
    state.dbm = 20;
    TEST_ASSERT_EQUAL_INT8(10, currentPower(&state, 10)); // Never above the ceiling.
    state.dbm = 2;
    TEST_ASSERT_EQUAL_INT8(2, currentPower(&state, 10));
    // A state taken under another configured power is dropped: a new USB setting applies.
    TEST_ASSERT_EQUAL_INT8(14, currentPower(&state, 14));
    TEST_ASSERT_EQUAL_INT8(0, currentPower(&state, 0));
    PowerState reset = nextPower(state, 14, true, KeepPower);
    TEST_ASSERT_EQUAL_INT8(14, reset.dbm);
    TEST_ASSERT_EQUAL_INT8(14, reset.ceiling);
    // Commands move the power within [MinPowerDbm, ceiling]; KeepPower keeps it.
    state = nextPower(state, 10, true, 5);
    TEST_ASSERT_EQUAL_INT8(5, state.dbm);
    state = nextPower(state, 10, true, KeepPower);
    TEST_ASSERT_EQUAL_INT8(5, state.dbm);
    state = nextPower(state, 10, true, 21);
    TEST_ASSERT_EQUAL_INT8(10, state.dbm);
    state = nextPower(state, 10, true, -128);
    TEST_ASSERT_EQUAL_INT8(MinPowerDbm, state.dbm);
    // Missed ACKs: after MissedAckLimit cycles the node returns to the ceiling.
    for (uint8_t cycle = 1; cycle < MissedAckLimit; ++cycle) {
        state = nextPower(state, 10, false, 3); // A command without an ACK is not taken.
        TEST_ASSERT_EQUAL_INT8(MinPowerDbm, state.dbm);
        TEST_ASSERT_EQUAL_UINT8(cycle, state.missed);
    }
    state = nextPower(state, 10, false, KeepPower);
    TEST_ASSERT_EQUAL_INT8(10, state.dbm);
    state = nextPower(state, 10, false, KeepPower);
    TEST_ASSERT_EQUAL_UINT8(MissedAckLimit, state.missed); // Saturates.
    state = nextPower(state, 10, true, KeepPower);
    TEST_ASSERT_EQUAL_UINT8(0, state.missed);
    state = nextPower(state, 99, true, 30); // Invalid ceiling falls back to the default.
    TEST_ASSERT_EQUAL_INT8(DefaultPowerDbm, state.dbm);
    TEST_ASSERT_TRUE(validPower(MinPowerDbm) && validPower(MaxPowerDbm));
    TEST_ASSERT_FALSE(validPower(MinPowerDbm - 1) || validPower(MaxPowerDbm + 1));
}
} // namespace

void runDeviceTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_transmitter_boot_modes);
    RUN_TEST(test_receiver_boot_modes);
    RUN_TEST(test_fault_retry_delay_doubles_up_to_a_bound);
    RUN_TEST(test_power_record_round_trips_and_fails_closed);
    RUN_TEST(test_power_follows_commands_within_the_ceiling_and_falls_back);
}
