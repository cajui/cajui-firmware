// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <string>
#include "storage_support.h"
#include "assertions.h"
#include "cajui_provisioning.h"
using namespace cajui;
using namespace fixtures;
namespace {
void command(Provisioning& admin, const char* input, const char* expected, UNITY_LINE_TYPE line) {
    char out[ReplyCapacity]{};
    UNITY_TEST_ASSERT(admin.execute(input, std::strlen(input), out, sizeof(out)), line,
                      "Reply buffer rejected");
    UNITY_TEST_ASSERT_EQUAL_STRING(expected, out, line, nullptr);
}
#define COMMAND(admin, input, expected) command(admin, input, expected, __LINE__)
const char* prepare = "CJ1 PREPARE 0000000000000002 000000000000002a 0000000000000001 "
                      "0000000000000002 000000000000000a 01010101010101010101010101010101 0001";
void test_usb_enrollment_resumes_without_resetting_counter() {
    MemoryRecords blob;
    auto store = mounted(blob);
    Provisioning admin(*store);
    COMMAND(
        admin, "CJ1 HELLO",
        "CJ1 OK HELLO 0000000000000002 tx ready 0000000000000000 0000000000000000 0000 0 00000001 "
        "admin");
    COMMAND(admin, prepare, "CJ1 OK PREPARE");
    COMMAND(admin, "CJ1 INFO 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK INFO 1 0000000000000000");
    COMMAND(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK ACTIVATE");
    COMMAND(admin, "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK RESERVE 0000000000000001");
    store.reset();
    store = mounted(blob);
    Provisioning rebooted(*store);
    COMMAND(rebooted, prepare, "CJ1 OK PREPARE");
    COMMAND(rebooted, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK ACTIVATE");
    COMMAND(rebooted, "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK RESERVE 0000000000000002");
    COMMAND(rebooted, "CJ1 REBOOT 0000000000000002", "CJ1 OK REBOOT");
    TEST_ASSERT_TRUE(rebooted.restartRequested());
}
void test_usb_parser_rejects_untrusted_input_without_echo() {
    MemoryRecords blob;
    auto store = mounted(blob);
    Provisioning admin(*store);
    const char* invalid[] = {"",
                             "CJ1",
                             "CJ1  HELLO",
                             "CJ1 HELLO ",
                             "CJ2 HELLO",
                             "CJ1 HELLO extra",
                             "CJ1 REBOOT 0000000000000003",
                             "CJ1 REBOOT 000000000000000G",
                             "CJ1 REBOOT 2",
                             "CJ1 UNKNOWN 0000000000000002",
                             "CJ1 A B C D E F G H I J",
                             "CJ1\tHELLO",
                             "CJ1 INFO 0000000000000002 BAD 0000000000000001"};
    for (auto input : invalid) {
        UNITY_SET_DETAIL(input);
        COMMAND(admin, input, "CJ1 ERR INVALID");
    }
    char out[ReplyCapacity]{};
    TEST_ASSERT_FALSE(admin.execute("CJ1 HELLO", 9, out, 10));
    TEST_ASSERT_TRUE(admin.execute(nullptr, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("CJ1 ERR INVALID", out);
    const char binary[] = "CJ1 HELLO\0hidden";
    TEST_ASSERT_TRUE(admin.execute(binary, sizeof(binary) - 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("CJ1 ERR INVALID", out);
    std::string longInput(CommandCapacity, 'a');
    TEST_ASSERT_TRUE(admin.execute(longInput.data(), longInput.size(), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("CJ1 ERR INVALID", out);
    std::string badKey(prepare);
    badKey[badKey.find("010101")] = 'z';
    COMMAND(admin, badKey.c_str(), "CJ1 ERR INVALID");
    TEST_ASSERT_EQUAL_UINT32(0, blob.writes);
}
void test_usb_revocation_storage_errors_and_no_secret_readback() {
    MemoryRecords blob;
    auto store = mounted(blob);
    Provisioning admin(*store);
    COMMAND(admin, "CJ1 INFO 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 ERR NOT_FOUND");
    COMMAND(admin, prepare, "CJ1 OK PREPARE");
    COMMAND(admin, "CJ1 REVOKE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK REVOKE");
    COMMAND(admin, prepare, "CJ1 ERR CONFLICT");
    COMMAND(admin, "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 ERR CONFLICT");
    blob.failRead = true;
    TEST_ASSERT_FALSE(store->mount());
    COMMAND(admin, prepare, "CJ1 ERR STORAGE");
    char out[ReplyCapacity]{};
    admin.execute("CJ1 HELLO", 9, out, sizeof(out));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "tx read"));
    TEST_ASSERT_NULL(std::strstr(out, "0101010101"));
}
void test_usb_reboot_remains_available_after_storage_failure() {
    MemoryRecords blob;
    auto store = mounted(blob);
    TEST_ASSERT_TRUE(enroll(*store));
    Provisioning admin(*store);
    blob.failAfter = true;
    COMMAND(admin, "CJ1 REVOKE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 ERR STORAGE");
    COMMAND(admin, "CJ1 INFO 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 ERR STORAGE");
    COMMAND(admin, "CJ1 REBOOT 0000000000000003", "CJ1 ERR INVALID");
    TEST_ASSERT_FALSE(admin.restartRequested());
    COMMAND(admin, "CJ1 REBOOT 0000000000000002", "CJ1 OK REBOOT");
    TEST_ASSERT_TRUE(admin.restartRequested());
    store.reset();
    store = mounted(blob);
    TEST_ASSERT_TRUE(store->healthy());
}
void test_usb_unknown_enrollment_is_not_found() {
    MemoryRecords blob;
    auto store = mounted(blob);
    Provisioning admin(*store);
    COMMAND(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 ERR NOT_FOUND");
    COMMAND(admin, prepare, "CJ1 OK PREPARE");
    COMMAND(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000b",
            "CJ1 ERR NOT_FOUND");
    COMMAND(admin, "CJ1 REVOKE 0000000000000002 0000000000000002 000000000000000b",
            "CJ1 ERR NOT_FOUND");
    TEST_ASSERT_EQUAL_UINT32(1, blob.writes);
}
void test_usb_reserve_reports_why_it_was_refused() {
    const char* reserve = "CJ1 RESERVE 0000000000000002 0000000000000002 000000000000000a";
    MemoryRecords blob;
    auto store = mounted(blob);
    Provisioning admin(*store);
    COMMAND(admin, reserve, "CJ1 ERR NOT_FOUND");
    COMMAND(admin, prepare, "CJ1 OK PREPARE");
    COMMAND(admin, reserve, "CJ1 ERR CONFLICT");
    COMMAND(admin, "CJ1 ACTIVATE 0000000000000002 0000000000000002 000000000000000a",
            "CJ1 OK ACTIVATE");
    blob.failBefore = true;
    COMMAND(admin, reserve, "CJ1 ERR STORAGE");
    MemoryRecords rxBlob;
    auto rx = mounted(rxBlob, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*rx));
    Provisioning receiver(*rx);
    COMMAND(receiver, "CJ1 RESERVE 0000000000000001 0000000000000002 000000000000000a",
            "CJ1 ERR INVALID");
}
std::string health(PersistentStore& store) {
    Provisioning admin(store);
    char out[ReplyCapacity]{};
    admin.execute("CJ1 HELLO", 9, out, sizeof(out));
    const std::string reply(out); // CJ1 OK HELLO <device> <role> <health> ...
    size_t start = 0;
    for (int field = 0; field < 5; ++field) start = reply.find(' ', start) + 1;
    return reply.substr(start, reply.find(' ', start) - start);
}
void test_usb_hello_reports_why_storage_is_unavailable() {
    MemoryRecords blob;
    auto store = mounted(blob);
    TEST_ASSERT_TRUE(enroll(*store));
    TEST_ASSERT_EQUAL_STRING("ready", health(*store).c_str());
    const auto good = blob.bytes("registry");
    std::unique_ptr<PersistentStore> other(new PersistentStore(blob, Role::Transmitter, 2));
    TEST_ASSERT_EQUAL_STRING("unmounted", health(*other).c_str());
    other.reset(new PersistentStore(blob, Role::Transmitter, 0));
    other->mount();
    TEST_ASSERT_EQUAL_STRING("identity", health(*other).c_str());
    other.reset(new PersistentStore(blob, Role::Transmitter, 3));
    other->mount();
    TEST_ASSERT_EQUAL_STRING("device", health(*other).c_str());
    other = mounted(blob, Role::Receiver);
    TEST_ASSERT_EQUAL_STRING("role", health(*other).c_str());
    blob.bytes("registry")[1] = 2; // Record version.
    repairChecksum(blob.bytes("registry"));
    other = mounted(blob);
    TEST_ASSERT_EQUAL_STRING("format", health(*other).c_str());
    blob.bytes("registry") = good;
    blob.bytes("registry")[39] = 0; // Entry state.
    repairChecksum(blob.bytes("registry"));
    other = mounted(blob);
    TEST_ASSERT_EQUAL_STRING("invalid", health(*other).c_str());
    blob.bytes("registry") = good;
    blob.bytes("registry")[20] ^= 1;
    other = mounted(blob);
    TEST_ASSERT_EQUAL_STRING("corrupt", health(*other).c_str());
    blob.bytes("registry") = good;
    blob.failRead = true;
    other = mounted(blob);
    TEST_ASSERT_EQUAL_STRING("read", health(*other).c_str());
    blob.failRead = false;
    other = mounted(blob);
    blob.failAfter = true;
    uint64_t counter = 0;
    TEST_ASSERT_FALSE(other->reserve(binding(), counter));
    TEST_ASSERT_EQUAL_STRING("write", health(*other).c_str());
}
}
void test_operation_console_allows_only_queries_and_admin_restart() {
    MemoryRecords blob;
    MemoryBlob settings;
    auto store = mounted(blob, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    Provisioning run(*store, 7, &settings, ConsoleMode::Operation);
    COMMAND(run, "CJ1 HELLO",
            "CJ1 OK HELLO 0000000000000001 rx ready 000000000000002a 0000000000000001 0001 0 "
            "00000007 run");
    COMMAND(run, "CJ1 INFO 0000000000000001 0000000000000002 000000000000000a",
            "CJ1 OK INFO 2 0000000000000000");
    COMMAND(run, "CJ1 UPLINKINFO 0000000000000001", "CJ1 OK UPLINKINFO 0");
    const char* prepareAnother = "CJ1 PREPARE 0000000000000001 000000000000002a 0000000000000001 "
                                 "0000000000000003 000000000000000b "
                                 "02020202020202020202020202020202 0001";
    const char* refused[] = {
        "CJ1 REVOKE 0000000000000001 0000000000000002 000000000000000a",
        "CJ1 ACTIVATE 0000000000000001 0000000000000002 000000000000000a",
        "CJ1 RESERVE 0000000000000001 0000000000000002 000000000000000a",
        prepareAnother,
        "CJ1 UPLINKSET 0000000000000001 ssid 41",
        "CJ1 UPLINKSAVE 0000000000000001",
    };
    for (auto input : refused) {
        UNITY_SET_DETAIL(input);
        COMMAND(run, input, "CJ1 ERR ADMIN");
    }
    TEST_ASSERT_EQUAL_size_t(0, settings.writes);
    TEST_ASSERT_FALSE(run.restartRequested());
    COMMAND(run, "CJ1 ADMIN 0000000000000002", "CJ1 ERR INVALID"); // Another device.
    COMMAND(run, "CJ1 ADMIN 0000000000000001", "CJ1 OK ADMIN");
    TEST_ASSERT_TRUE(run.restartRequested());
    TEST_ASSERT_TRUE(run.adminRequested());
    Provisioning admin(*store);
    COMMAND(admin, "CJ1 REBOOT 0000000000000001", "CJ1 OK REBOOT");
    TEST_ASSERT_TRUE(admin.restartRequested());
    TEST_ASSERT_FALSE(admin.adminRequested()); // REBOOT returns to operation.
    Provisioning again(*store);
    COMMAND(again, "CJ1 ADMIN 0000000000000001", "CJ1 OK ADMIN"); // Restart staying in admin.
    TEST_ASSERT_TRUE(again.adminRequested());
    TEST_ASSERT_FALSE(again.pairRequested());
    Provisioning receiverPair(*store, 1, nullptr, ConsoleMode::Operation);
    COMMAND(receiverPair, "CJ1 PAIR 0000000000000001", "CJ1 ERR INVALID"); // Receivers host.
    TEST_ASSERT_FALSE(receiverPair.restartRequested());
    MemoryRecords txBlob;
    auto transmitter = mounted(txBlob);
    Provisioning pair(*transmitter, 1, nullptr, ConsoleMode::Operation);
    COMMAND(pair, "CJ1 PAIR 0000000000000002", "CJ1 OK PAIR");
    TEST_ASSERT_TRUE(pair.restartRequested());
    TEST_ASSERT_TRUE(pair.pairRequested());
    TEST_ASSERT_FALSE(pair.adminRequested());
}
void test_usb_reset_leaves_the_network_only_in_admin_mode() {
    MemoryRecords blob;
    auto store = mounted(blob, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    Frame ack{};
    EXPECT_RESULT(Result::Ok, receive(binding(), data(1), *store, ack));
    Provisioning run(*store, 1, nullptr, ConsoleMode::Operation);
    COMMAND(run, "CJ1 RESET 0000000000000001", "CJ1 ERR ADMIN");
    Provisioning admin(*store);
    COMMAND(admin, "CJ1 RESET 0000000000000001", "CJ1 ERR QUEUED");
    COMMAND(admin, "CJ1 RESET 0000000000000001 all", "CJ1 ERR INVALID");
    COMMAND(admin, "CJ1 RESET 0000000000000002 discard", "CJ1 ERR INVALID");
    COMMAND(admin, "CJ1 RESET 0000000000000001 discard", "CJ1 OK RESET");
    COMMAND(admin, "CJ1 HELLO",
            "CJ1 OK HELLO 0000000000000001 rx ready 0000000000000000 0000000000000000 0000 0 "
            "00000001 admin");
    COMMAND(admin, "CJ1 RESET 0000000000000001", "CJ1 OK RESET"); // Idempotent.
    blob.failBefore = true;
    COMMAND(admin, "CJ1 RESET 0000000000000001", "CJ1 ERR STORAGE");
}
void runProvisioningTests() {
    UnitySetTestFile(__FILE__); // UNITY_BEGIN runs in test_main.cpp.
    RUN_TEST(test_usb_enrollment_resumes_without_resetting_counter);
    RUN_TEST(test_usb_parser_rejects_untrusted_input_without_echo);
    RUN_TEST(test_usb_revocation_storage_errors_and_no_secret_readback);
    RUN_TEST(test_usb_reboot_remains_available_after_storage_failure);
    RUN_TEST(test_usb_unknown_enrollment_is_not_found);
    RUN_TEST(test_usb_reserve_reports_why_it_was_refused);
    RUN_TEST(test_usb_hello_reports_why_storage_is_unavailable);
    RUN_TEST(test_operation_console_allows_only_queries_and_admin_restart);
    RUN_TEST(test_usb_reset_leaves_the_network_only_in_admin_mode);
}
