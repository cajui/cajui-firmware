#include <unity.h>
#include "storage_support.h"
#include "assertions.h"
using namespace cajui;
using namespace fixtures;
namespace {
void test_durable_enrollment_and_counter_restart() {
    MemoryBlob blob; auto store = mounted(blob);
    TEST_ASSERT_TRUE(store->healthy()); Binding b{};
    TEST_ASSERT_FALSE(store->binding(2, b));
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(), 1));
    TEST_ASSERT_FALSE(store->binding(2, b)); store.reset(); store = mounted(blob);
    EnrollmentInfo info{}; TEST_ASSERT_TRUE(store->info(2, 10, info));
    TEST_ASSERT_EQUAL_INT(int(Enrollment::Prepared), int(info.state));
    EXPECT_RESULT(Result::Ok, store->activate(2, 10)); TEST_ASSERT_TRUE(store->binding(2, b));
    uint64_t counter = 0; TEST_ASSERT_TRUE(store->reserve(b, counter)); TEST_ASSERT_EQUAL_UINT64(1, counter);
    store.reset(); store = mounted(blob);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 10));
    TEST_ASSERT_TRUE(store->reserve(b, counter)); TEST_ASSERT_EQUAL_UINT64(2, counter);
    TEST_ASSERT_EQUAL_UINT64(42, store->network()); TEST_ASSERT_EQUAL_UINT64(1, store->receiver());
}
void test_uncertain_counter_write_cannot_reuse_nonce() {
    MemoryBlob blob; auto store = mounted(blob); TEST_ASSERT_TRUE(enroll(*store));
    auto b = binding(); uint64_t counter = 99;
    blob.failAfter = true; TEST_ASSERT_FALSE(store->reserve(b, counter)); TEST_ASSERT_EQUAL_UINT64(0, counter);
    TEST_ASSERT_FALSE(store->healthy()); TEST_ASSERT_FALSE(store->reserve(b, counter));
    blob.failAfter = false; store.reset(); store = mounted(blob);
    TEST_ASSERT_TRUE(store->reserve(b, counter)); TEST_ASSERT_EQUAL_UINT64(2, counter);
}
void test_failed_counter_write_and_corruption_fail_closed() {
    MemoryBlob blob; auto store = mounted(blob); TEST_ASSERT_TRUE(enroll(*store));
    blob.failBefore = true; uint64_t counter = 0; TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    EXPECT_RESULT(Result::StorageError, store->activate(2, 10));
    blob.failBefore = false; store.reset(); store = mounted(blob);
    TEST_ASSERT_TRUE(store->reserve(binding(), counter)); TEST_ASSERT_EQUAL_UINT64(1, counter);
    blob.tear = true; TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    store.reset(); store = mounted(blob); TEST_ASSERT_FALSE(store->healthy());
    EXPECT_RESULT(Result::StorageError, store->prepare(42, 1, 2, 10, key(), 1));
    blob.failRead = true; TEST_ASSERT_FALSE(store->mount());
}
void test_receiver_commit_restart_duplicate_and_drain() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(enroll(*store));
    auto frame = data(1); Frame ack{};
    EXPECT_RESULT(Result::Ok, receive(binding(), frame, *store, ack)); TEST_ASSERT_GREATER_THAN_UINT32(0, ack.size);
    store.reset(); store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(store->healthy());
    EXPECT_RESULT(Result::Duplicate, receive(binding(), frame, *store, ack)); TEST_ASSERT_EQUAL_UINT32(1, store->queued());
    QueuedSample q{}; TEST_ASSERT_TRUE(store->peek(q)); TEST_ASSERT_EQUAL_INT32(25000, q.data.readings[0].milliValue);
    EXPECT_RESULT(Result::Conflict, store->forwarded(2, 10, 2));
    EXPECT_RESULT(Result::Ok, store->forwarded(q.node, q.generation, q.counter));
    store.reset(); store = mounted(blob, Role::Receiver); TEST_ASSERT_EQUAL_UINT32(0, store->queued());
    EXPECT_RESULT(Result::Duplicate, receive(binding(), frame, *store, ack));
    TEST_ASSERT_EQUAL_UINT32(0, store->queued()); TEST_ASSERT_FALSE(store->peek(q));
}
void test_commit_ambiguity_after_power_loss_is_recovered() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(enroll(*store)); Frame ack{};
    blob.failAfter = true; EXPECT_RESULT(Result::StorageError, receive(binding(), data(1), *store, ack));
    TEST_ASSERT_EQUAL_UINT32(0, ack.size); blob.failAfter = false;
    store.reset(); store = mounted(blob, Role::Receiver);
    EXPECT_RESULT(Result::Duplicate, receive(binding(), data(1), *store, ack)); TEST_ASSERT_EQUAL_UINT32(1, store->queued());
}
void test_queue_capacity_multiple_nodes_and_no_partial_receipt() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store)); TEST_ASSERT_TRUE(enroll(*store, 3, 11, 2)); Frame ack{};
    for (size_t i = 1; i <= QueueCapacity; ++i) {
        SCENARIO(i);
        EXPECT_RESULT(Result::Ok, receive(binding(), data(i), *store, ack));
    }
    EXPECT_RESULT(Result::Full, receive(binding(3, 2), data(1, 3, 2), *store, ack));
    TEST_ASSERT_EQUAL_UINT32(0, ack.size);
    Receipt receipt{}; TEST_ASSERT_TRUE(store->load(binding(3, 2), receipt)); TEST_ASSERT_EQUAL_UINT64(0, receipt.counter);
    EXPECT_RESULT(Result::Duplicate, receive(binding(), data(QueueCapacity), *store, ack));
    store.reset(); store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(store->healthy());
    EXPECT_RESULT(Result::Ok, store->forwarded(2, 10, 1));
    EXPECT_RESULT(Result::Ok, receive(binding(3, 2), data(1, 3, 2), *store, ack));
    TEST_ASSERT_EQUAL_UINT32(QueueCapacity, store->queued());
}
void test_rotation_revocation_and_retired_keys_are_persistent() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(enroll(*store)); Frame ack{};
    EXPECT_RESULT(Result::Ok, receive(binding(), data(1), *store, ack));
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 11));
    Receipt receipt{}; TEST_ASSERT_FALSE(store->load(binding(), receipt));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 12, key(1), 1));
    EXPECT_RESULT(Result::Conflict, store->activate(2, 10));
    QueuedSample q{}; TEST_ASSERT_TRUE(store->peek(q)); TEST_ASSERT_EQUAL_UINT64(10, q.generation);
    store.reset(); store = mounted(blob, Role::Receiver);
    Binding b{}; TEST_ASSERT_TRUE(store->binding(2, b)); TEST_ASSERT_EQUAL_HEX8_ARRAY(key(2).data(), b.key.data(), 16);
    EXPECT_RESULT(Result::Ok, store->revoke(2, 11)); EXPECT_RESULT(Result::Ok, store->revoke(2, 11));
    TEST_ASSERT_FALSE(store->binding(2, b)); TEST_ASSERT_TRUE(store->peek(q));
}
void test_bad_provisioning_and_identity_do_not_mutate_state() {
    MemoryBlob blob; auto store = mounted(blob);
    EXPECT_RESULT(Result::Invalid, store->prepare(0, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 2, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 3, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 2, 0, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 2, 10, key(0), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 2, 10, key(), 2));
    TEST_ASSERT_EQUAL_UINT32(0, blob.writes); TEST_ASSERT_TRUE(enroll(*store));
    EXPECT_RESULT(Result::Conflict, store->prepare(43, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 10, key(2), 1));
    EXPECT_RESULT(Result::NotFound, store->activate(2, 100)); EXPECT_RESULT(Result::NotFound, store->revoke(2, 100));
    store.reset(); store = mounted(blob, Role::Receiver); TEST_ASSERT_FALSE(store->healthy());
}
void test_full_binding_registry_and_prepared_conflict() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 3, 10, key(2), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 10));
    for (size_t i = 1; i < BindingCapacity; ++i) TEST_ASSERT_TRUE(enroll(*store, i + 2, i + 10, uint8_t(i + 1)));
    EXPECT_RESULT(Result::Full, store->prepare(42, 1, 100, 100, key(100), 1));
    store.reset(); store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(store->healthy());
}
void repairChecksum(MemoryBlob& blob) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < blob.size - 4; ++i) {
        crc ^= blob.bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    crc = ~crc;
    for (size_t i = 0; i < 4; ++i) blob.bytes[blob.size - 4 + i] = uint8_t(crc >> ((3 - i) * 8));
}
void test_semantically_invalid_snapshots_fail_closed_even_with_valid_crc() {
    MemoryBlob blob; auto store = mounted(blob); TEST_ASSERT_TRUE(enroll(*store));
    uint64_t value = 0; TEST_ASSERT_TRUE(store->reserve(binding(), value)); store.reset();
    const auto good = blob.bytes;
    struct Mutation { size_t offset; uint8_t value; };
    const Mutation invalid[] = {{0, 0}, {4, 2}, {5, 2}, {13, 3}, {21, 0}, {29, 0}, {37, 0},
        {39, 2}, {40, 255}, {41, 0}, {41, 17}, {42, 4}, {42, 0}, {42, 1}, {50, 0}, {50, 3},
        {58, 0}, {91, 1}, {90, 1}, {92, 1}};
    for (const auto& mutation : invalid) {
        SCENARIO(mutation.offset);
        blob.bytes = good; blob.bytes[mutation.offset] = mutation.value; repairChecksum(blob);
        store = mounted(blob); TEST_ASSERT_FALSE_MESSAGE(store->healthy(), "Invalid snapshot accepted"); store.reset();
    }
    blob.bytes = good;
    for (size_t i = 59; i < 75; ++i) blob.bytes[i] = 0;
    repairChecksum(blob); store = mounted(blob); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    blob.bytes = good; blob.size = 12; store = mounted(blob); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    blob.size = SnapshotSize + 1; store = mounted(blob); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    std::unique_ptr<PersistentStore> invalidRole(new PersistentStore(blob, Role(3), 2));
    TEST_ASSERT_FALSE(invalidRole->mount());
}
void test_corrupt_receiver_receipts_and_queued_frames_are_rejected() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver); TEST_ASSERT_TRUE(enroll(*store));
    Frame ack{}; EXPECT_RESULT(Result::Ok, receive(binding(), data(1), *store, ack)); store.reset();
    const auto good = blob.bytes;
    // Header receiver, tx-only counter, receipt, queue index/size and authenticated frame.
    const size_t invalidOffsets[] = {37, 82, 90, 92, 93, 228, 229, 231};
    for (size_t offset : invalidOffsets) {
        SCENARIO(offset);
        blob.bytes = good; blob.bytes[offset] ^= 0xff; repairChecksum(blob);
        store = mounted(blob, Role::Receiver); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    }
    blob.bytes = good; blob.bytes[90] = 0; repairChecksum(blob);
    store = mounted(blob, Role::Receiver); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    blob.bytes = good; blob.bytes[228] = 15; repairChecksum(blob);
    store = mounted(blob, Role::Receiver); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    blob.bytes = good; auto later = data(2);
    std::memcpy(blob.bytes.data() + 231, later.bytes.data(), later.size); repairChecksum(blob);
    store = mounted(blob, Role::Receiver); TEST_ASSERT_FALSE(store->healthy()); store.reset();
    blob.bytes = good; blob.bytes[92] = uint8_t(ack.size);
    std::memcpy(blob.bytes.data() + 93, ack.bytes.data(), ack.size); repairChecksum(blob);
    store = mounted(blob, Role::Receiver); TEST_ASSERT_FALSE(store->healthy());
}
void test_duplicate_persistent_credentials_and_active_nodes_are_rejected() {
    MemoryBlob blob; auto store = mounted(blob, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store)); TEST_ASSERT_TRUE(enroll(*store, 3, 11, 2)); store.reset();
    const auto good = blob.bytes;
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCENARIO(scenario);
        blob.bytes = good;
        if (scenario == 0) std::memcpy(blob.bytes.data() + 245, blob.bytes.data() + 59, 16);
        if (scenario == 1) blob.bytes[244] = 10;
        if (scenario == 2) blob.bytes[236] = 2;
        repairChecksum(blob); store = mounted(blob, Role::Receiver);
        TEST_ASSERT_FALSE(store->healthy()); store.reset();
    }
}
void test_counter_revision_exhaustion_and_operation_guards() {
    MemoryBlob blob; auto store = mounted(blob); TEST_ASSERT_TRUE(enroll(*store)); store.reset();
    for (size_t i = 75; i < 83; ++i) blob.bytes[i] = 0xff;
    repairChecksum(blob); store = mounted(blob); TEST_ASSERT_TRUE(store->healthy());
    uint64_t counter = 1; TEST_ASSERT_FALSE(store->reserve(binding(), counter)); TEST_ASSERT_EQUAL_UINT64(0, counter);
    store.reset(); for (size_t i = 14; i < 22; ++i) blob.bytes[i] = 0xff;
    repairChecksum(blob); store = mounted(blob); TEST_ASSERT_TRUE(store->healthy());
    EXPECT_RESULT(Result::StorageError, store->revoke(2, 10));
    EXPECT_RESULT(Result::StorageError, store->revoke(2, 10));
    EXPECT_RESULT(Result::StorageError, store->forwarded(2, 10, 1));
    Binding b{}; EnrollmentInfo info{}; TEST_ASSERT_FALSE(store->binding(2, b)); TEST_ASSERT_FALSE(store->info(2, 10, info));
    store.reset(); MemoryBlob receiverBlob; store = mounted(receiverBlob, Role::Receiver);
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 3, 2, 10, key(), 1)); TEST_ASSERT_TRUE(enroll(*store));
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    Receipt receipt{}; receipt.counter = 1; receipt.last = data(1);
    EXPECT_RESULT(Result::Conflict, store->commit(binding(), 1, receipt, sample()));
    receipt.counter = 2; EXPECT_RESULT(Result::Invalid, store->commit(binding(), 0, receipt, sample()));
    EXPECT_RESULT(Result::Unauthorized, store->commit(binding(3), 0, receipt, sample()));
    auto wrong = binding(); wrong.active = false; TEST_ASSERT_FALSE(store->load(wrong, receipt));
    wrong = binding(); wrong.network = 99; TEST_ASSERT_FALSE(store->load(wrong, receipt));
    EXPECT_RESULT(Result::Conflict, store->forwarded(2, 10, 1));
}

}
void runStorageTests() {
    UnitySetTestFile(__FILE__); // UNITY_BEGIN runs in test_main.cpp.
    RUN_TEST(test_durable_enrollment_and_counter_restart);
    RUN_TEST(test_uncertain_counter_write_cannot_reuse_nonce);
    RUN_TEST(test_failed_counter_write_and_corruption_fail_closed);
    RUN_TEST(test_receiver_commit_restart_duplicate_and_drain);
    RUN_TEST(test_commit_ambiguity_after_power_loss_is_recovered);
    RUN_TEST(test_queue_capacity_multiple_nodes_and_no_partial_receipt);
    RUN_TEST(test_rotation_revocation_and_retired_keys_are_persistent);
    RUN_TEST(test_bad_provisioning_and_identity_do_not_mutate_state);
    RUN_TEST(test_full_binding_registry_and_prepared_conflict);
    RUN_TEST(test_semantically_invalid_snapshots_fail_closed_even_with_valid_crc);
    RUN_TEST(test_corrupt_receiver_receipts_and_queued_frames_are_rejected);
    RUN_TEST(test_duplicate_persistent_credentials_and_active_nodes_are_rejected);
    RUN_TEST(test_counter_revision_exhaustion_and_operation_guards);
}
