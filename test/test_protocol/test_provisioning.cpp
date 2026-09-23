#include <unity.h>
#include <string>
#include "storage_support.h"
#include "cajui_provisioning.h"
using namespace cajui;
using namespace fixtures;
namespace {
void command(Provisioning& admin, const char* input, const char* expected) {
    char out[ReplyCapacity]{};
    TEST_ASSERT_TRUE(admin.execute(input, std::strlen(input), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(expected, out);
}
const char* prepare = "CJ1 PREPARE 0000000000000002 000000000000002a 0000000000000001 0000000000000002 000000000000000a 01010101010101010101010101010101 0001";
void test_usb_enrollment_resumes_without_resetting_counter() {
    MemoryBlob blob; auto store = mounted(blob); Provisioning admin(*store);
    command(admin, "CJ1 HELLO", "CJ1 OK HELLO 0000000000000002 tx ready 0000000000000000 0000000000000000 0000 0 00000001");
    command(admin, prepare, "CJ1 OK PREPARE");
    command(admin, "CJ1 INFO 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK INFO 1 0000000000000000");
    command(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK ACTIVATE");
    command(admin, "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK RESERVE 0000000000000001");
    store.reset(); store = mounted(blob); Provisioning rebooted(*store);
    command(rebooted, prepare, "CJ1 OK PREPARE");
    command(rebooted, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK ACTIVATE");
    command(rebooted, "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK RESERVE 0000000000000002");
    command(rebooted, "CJ1 REBOOT 0000000000000002", "CJ1 OK REBOOT"); TEST_ASSERT_TRUE(rebooted.restartRequested());
}
void test_usb_parser_rejects_untrusted_input_without_echo() {
    MemoryBlob blob; auto store = mounted(blob); Provisioning admin(*store);
    const char* invalid[] = {"", "CJ1", "CJ1  HELLO", "CJ1 HELLO ", "CJ2 HELLO", "CJ1 HELLO extra",
        "CJ1 REBOOT 0000000000000003", "CJ1 REBOOT 000000000000000G", "CJ1 REBOOT 2",
        "CJ1 UNKNOWN 0000000000000002", "CJ1 A B C D E F G H I J", "CJ1\tHELLO",
        "CJ1 INFO 0000000000000002 BAD 0000000000000001"};
    for (auto input : invalid) command(admin, input, "CJ1 ERR INVALID");
    char out[ReplyCapacity]{};
    TEST_ASSERT_FALSE(admin.execute("CJ1 HELLO", 9, out, 10));
    TEST_ASSERT_TRUE(admin.execute(nullptr, 1, out, sizeof(out))); TEST_ASSERT_EQUAL_STRING("CJ1 ERR INVALID", out);
    const char binary[] = "CJ1 HELLO\0hidden";
    TEST_ASSERT_TRUE(admin.execute(binary, sizeof(binary) - 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("CJ1 ERR INVALID", out);
    std::string longInput(CommandCapacity, 'a');
    TEST_ASSERT_TRUE(admin.execute(longInput.data(), longInput.size(), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("CJ1 ERR INVALID", out);
    std::string badKey(prepare); badKey[badKey.find("010101")] = 'z';
    command(admin, badKey.c_str(), "CJ1 ERR INVALID"); TEST_ASSERT_EQUAL_UINT32(0, blob.writes);
}
void test_usb_revocation_storage_errors_and_no_secret_readback() {
    MemoryBlob blob; auto store = mounted(blob); Provisioning admin(*store);
    command(admin, "CJ1 INFO 0000000000000002 0000000000000002 000000000000000a", "CJ1 ERR NOT_FOUND");
    command(admin, prepare, "CJ1 OK PREPARE");
    command(admin, "CJ1 REVOKE 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK REVOKE");
    command(admin, prepare, "CJ1 ERR CONFLICT");
    command(admin, "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a", "CJ1 ERR CONFLICT");
    blob.failRead = true; TEST_ASSERT_FALSE(store->mount());
    command(admin, prepare, "CJ1 ERR STORAGE");
    char out[ReplyCapacity]{}; admin.execute("CJ1 HELLO", 9, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "tx error")); TEST_ASSERT_NULL(std::strstr(out, "0101010101"));
}
void test_usb_reboot_remains_available_after_storage_failure() {
    MemoryBlob blob; auto store = mounted(blob); TEST_ASSERT_TRUE(enroll(*store));
    Provisioning admin(*store); blob.failAfter = true;
    command(admin, "CJ1 REVOKE 0000000000000002 0000000000000002 000000000000000a", "CJ1 ERR STORAGE");
    command(admin, "CJ1 INFO 0000000000000002 0000000000000002 000000000000000a", "CJ1 ERR STORAGE");
    command(admin, "CJ1 REBOOT 0000000000000003", "CJ1 ERR INVALID"); TEST_ASSERT_FALSE(admin.restartRequested());
    command(admin, "CJ1 REBOOT 0000000000000002", "CJ1 OK REBOOT"); TEST_ASSERT_TRUE(admin.restartRequested());
    store.reset(); store = mounted(blob); TEST_ASSERT_TRUE(store->healthy());
}
void test_usb_unknown_enrollment_is_not_found() {
    MemoryBlob blob; auto store = mounted(blob); Provisioning admin(*store);
    command(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a", "CJ1 ERR NOT_FOUND");
    command(admin, prepare, "CJ1 OK PREPARE");
    command(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000b", "CJ1 ERR NOT_FOUND");
    command(admin, "CJ1 REVOKE 0000000000000002 0000000000000002 000000000000000b", "CJ1 ERR NOT_FOUND");
    TEST_ASSERT_EQUAL_UINT32(1, blob.writes);
}
void test_usb_reserve_reports_why_it_was_refused() {
    const char* reserve = "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a";
    MemoryBlob blob; auto store = mounted(blob); Provisioning admin(*store);
    command(admin, reserve, "CJ1 ERR NOT_FOUND");
    command(admin, prepare, "CJ1 OK PREPARE"); command(admin, reserve, "CJ1 ERR CONFLICT");
    command(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a", "CJ1 OK ACTIVATE");
    blob.failBefore = true; command(admin, reserve, "CJ1 ERR STORAGE");
    MemoryBlob rxBlob; auto rx = mounted(rxBlob, Role::Receiver); TEST_ASSERT_TRUE(enroll(*rx));
    Provisioning receiver(*rx);
    command(receiver, "CJ1 RESERVE 0000000000000001 0000000000000002 000000000000000a", "CJ1 ERR INVALID");
}
}
void runProvisioningTests() {
    RUN_TEST(test_usb_enrollment_resumes_without_resetting_counter);
    RUN_TEST(test_usb_parser_rejects_untrusted_input_without_echo);
    RUN_TEST(test_usb_revocation_storage_errors_and_no_secret_readback);
    RUN_TEST(test_usb_reboot_remains_available_after_storage_failure);
    RUN_TEST(test_usb_unknown_enrollment_is_not_found);
    RUN_TEST(test_usb_reserve_reports_why_it_was_refused);
}
