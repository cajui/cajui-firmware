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
} // namespace

void runDeviceTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_transmitter_boot_modes);
    RUN_TEST(test_receiver_boot_modes);
    RUN_TEST(test_fault_retry_delay_doubles_up_to_a_bound);
}
