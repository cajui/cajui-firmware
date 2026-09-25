// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <type_traits>
#include "storage_support.h"
#include "assertions.h"
using namespace cajui;
using namespace fixtures;
namespace {
// Compile-time regressions: duplicating a store's cached reservation state is unsafe.
static_assert(!std::is_copy_constructible<PersistentStore>::value, "Store must not be copied");
static_assert(!std::is_copy_assignable<PersistentStore>::value, "Store must not be copy-assigned");
static_assert(!std::is_move_constructible<PersistentStore>::value, "Store must not be moved");
static_assert(!std::is_move_assignable<PersistentStore>::value, "Store must not be move-assigned");
#define EXPECT_HEALTH(expected, store) TEST_ASSERT_EQUAL_INT(int(expected), int((store).health()))
// Registry offsets (records.h): header, then the first entry.
constexpr size_t RegRole = 2, RegDevice = 10, RegRevision = 11, RegNetwork = 19, RegReceiver = 27,
                 RegProfile = 36, RegCount = 37, RegSlot = 38, RegState = 39, RegNode = 40,
                 RegGeneration = 48, RegKey = 56, RegCounter = 72, RegEntry = 42;
// Receipt and queue record offsets.
constexpr size_t ReceiptCounter = 10, ReceiptThrough = 18, ReceiptFrame = 28;
constexpr size_t QueueSequence = 2, QueueSlot = 10, QueueGeneration = 11, QueueFrame = 21;

void setNumber(std::vector<uint8_t>& bytes, size_t at, uint64_t value, size_t size = 8) {
    for (size_t i = 0; i < size; ++i) bytes[at + size - 1 - i] = uint8_t(value >> (8 * i));
}
std::unique_ptr<PersistentStore> remount(MemoryRecords& records, Role role = Role::Transmitter) {
    return mounted(records, role);
}
void receiveOk(PersistentStore& store, uint64_t counter, uint64_t node = 2, uint8_t secret = 1) {
    Frame ack{};
    EXPECT_RESULT(Result::Ok,
                  receive(binding(node, secret), data(counter, node, secret), store, ack));
}

void test_durable_enrollment_and_counter_restart() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(store->healthy());
    Binding b{};
    TEST_ASSERT_FALSE(store->binding(2, b));
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(), 1));
    TEST_ASSERT_FALSE(store->binding(2, b));
    store = remount(records);
    EnrollmentInfo info{};
    TEST_ASSERT_TRUE(store->info(2, 10, info));
    EXPECT_RESULT(Enrollment::Prepared, info.state);
    EXPECT_RESULT(Result::Ok, store->activate(2, 10));
    TEST_ASSERT_TRUE(store->binding(2, b));
    uint64_t counter = 0;
    TEST_ASSERT_TRUE(store->reserve(b, counter));
    TEST_ASSERT_EQUAL_UINT64(1, counter);
    store = remount(records);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 10));
    TEST_ASSERT_TRUE(store->reserve(b, counter));
    TEST_ASSERT_EQUAL_UINT64(2, counter);
    TEST_ASSERT_EQUAL_UINT64(42, store->network());
    TEST_ASSERT_EQUAL_UINT64(1, store->receiver());
    TEST_ASSERT_FALSE(records.has("snapshot")); // A new device never writes the v1 layout.
}
void test_uncertain_counter_write_cannot_reuse_nonce() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(enroll(*store));
    auto b = binding();
    uint64_t counter = 99;
    records.failAfter = true;
    TEST_ASSERT_FALSE(store->reserve(b, counter));
    TEST_ASSERT_EQUAL_UINT64(0, counter);
    TEST_ASSERT_FALSE(store->healthy());
    TEST_ASSERT_FALSE(store->reserve(b, counter));
    records.failAfter = false;
    store = remount(records);
    TEST_ASSERT_TRUE(store->reserve(b, counter));
    TEST_ASSERT_EQUAL_UINT64(2, counter);
}
void test_failed_counter_write_and_corruption_fail_closed() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(enroll(*store));
    records.failBefore = true;
    uint64_t counter = 0;
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    EXPECT_RESULT(Result::StorageError, store->activate(2, 10));
    records.failBefore = false;
    store = remount(records);
    TEST_ASSERT_TRUE(store->reserve(binding(), counter));
    TEST_ASSERT_EQUAL_UINT64(1, counter);
    records.tear = true;
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    records.tear = false;
    store = remount(records);
    EXPECT_HEALTH(Health::Corrupt, *store);
    EXPECT_RESULT(Result::StorageError, store->prepare(42, 1, 2, 10, key(), 1));
    records.failRead = true;
    TEST_ASSERT_FALSE(store->mount());
    EXPECT_HEALTH(Health::ReadError, *store);
}
void test_receiver_commit_restart_duplicate_and_drain() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    auto frame = data(1);
    Frame ack{};
    EXPECT_RESULT(Result::Ok, receive(binding(), frame, *store, ack));
    TEST_ASSERT_GREATER_THAN_UINT32(0, ack.size);
    store = remount(records, Role::Receiver);
    TEST_ASSERT_TRUE(store->healthy());
    EXPECT_RESULT(Result::Duplicate, receive(binding(), frame, *store, ack));
    TEST_ASSERT_EQUAL_UINT32(1, store->queued());
    QueuedSample q{};
    TEST_ASSERT_TRUE(store->peek(q));
    TEST_ASSERT_EQUAL_INT32(25000, q.data.readings[0].milliValue);
    EXPECT_RESULT(Result::Conflict, store->forwarded(2, 10, 2));
    EXPECT_RESULT(Result::Ok, store->forwarded(q.node, q.generation, q.counter));
    EXPECT_RESULT(Result::Conflict, store->forwarded(q.node, q.generation, q.counter));
    store = remount(records, Role::Receiver);
    TEST_ASSERT_EQUAL_UINT32(0, store->queued());
    EXPECT_RESULT(Result::Duplicate, receive(binding(), frame, *store, ack));
    TEST_ASSERT_EQUAL_UINT32(0, store->queued());
    TEST_ASSERT_FALSE(store->peek(q));
}
void test_each_sample_writes_two_small_records_whatever_the_state() {
    // Wear and ACK latency no longer grow with enrollments or queue depth.
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    for (uint64_t node = 2; node < 2 + BindingCapacity; ++node)
        TEST_ASSERT_TRUE(enroll(*store, node, node + 100, uint8_t(node)));
    for (uint64_t counter = 1; counter < 100; ++counter) receiveOk(*store, counter, 2, 2);
    const size_t before = records.writes;
    receiveOk(*store, 100, 2, 2);
    TEST_ASSERT_EQUAL_size_t(before + 2, records.writes);
    for (const auto& record : records.records) {
        UNITY_SET_DETAIL(record.first.c_str());
        if (record.first != "registry")
            TEST_ASSERT_LESS_OR_EQUAL(records::ReceiptCapacity, record.second.size());
    }
    QueuedSample q{};
    TEST_ASSERT_TRUE(store->peek(q));
    EXPECT_RESULT(Result::Ok, store->forwarded(q.node, q.generation, q.counter));
    TEST_ASSERT_EQUAL_size_t(before + 3, records.writes);
    TEST_ASSERT_EQUAL_size_t(records::HeadSize, records.bytes("head").size());
    MemoryRecords txRecords;
    auto tx = mounted(txRecords);
    TEST_ASSERT_TRUE(enroll(*tx));
    uint64_t counter = 0;
    TEST_ASSERT_TRUE(tx->reserve(binding(), counter));
    TEST_ASSERT_EQUAL_size_t(records::RegistryHeaderSize + records::RegistryEntrySize + 4,
                             txRecords.bytes("registry").size());
}
void test_power_loss_at_each_commit_step() {
    // 0: queue record lost; 1: queue record written, receipt lost (the orphan case);
    // 2: both written but the receipt write reported failure (ambiguous, durable).
    for (int step = 0; step < 3; ++step) {
        SCENARIO(step);
        MemoryRecords records;
        auto store = mounted(records, Role::Receiver);
        TEST_ASSERT_TRUE(enroll(*store));
        receiveOk(*store, 1);
        Frame ack{};
        if (step < 2)
            records.failAt = step;
        else
            records.ambiguousAt = 1;
        EXPECT_RESULT(Result::StorageError, receive(binding(), data(2), *store, ack));
        TEST_ASSERT_EQUAL_UINT32(0, ack.size); // Never an ACK without a known commit.
        TEST_ASSERT_FALSE(store->healthy());
        store = remount(records, Role::Receiver);
        TEST_ASSERT_TRUE(store->healthy());
        if (step < 2) {
            TEST_ASSERT_EQUAL_UINT32(1, store->queued()); // An orphan record is not a sample.
            EXPECT_RESULT(Result::Ok, receive(binding(), data(2), *store, ack));
        } else {
            TEST_ASSERT_EQUAL_UINT32(2, store->queued());
            EXPECT_RESULT(Result::Duplicate, receive(binding(), data(2), *store, ack));
        }
        store = remount(records, Role::Receiver);
        TEST_ASSERT_EQUAL_UINT32(2, store->queued());
        const uint64_t expected[] = {1, 2};
        for (uint64_t counter : expected) {
            QueuedSample q{};
            TEST_ASSERT_TRUE(store->peek(q));
            TEST_ASSERT_EQUAL_UINT64(counter, q.counter);
            EXPECT_RESULT(Result::Ok, store->forwarded(2, 10, counter));
        }
    }
}
void test_ambiguous_head_write_repeats_at_most_one_sample() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    receiveOk(*store, 1);
    receiveOk(*store, 2);
    for (int durable = 0; durable < 2; ++durable) {
        SCENARIO(durable);
        if (durable)
            records.ambiguousAt = 0;
        else
            records.failAt = 0;
        EXPECT_RESULT(Result::StorageError, store->forwarded(2, 10, 1));
        store = remount(records, Role::Receiver);
        QueuedSample q{};
        TEST_ASSERT_TRUE(store->peek(q));
        // Not durable: the same sample again (Central deduplicates). Durable: the next one.
        TEST_ASSERT_EQUAL_UINT64(durable ? 2 : 1, q.counter);
    }
}
void test_queue_capacity_wraps_and_survives_restart() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    TEST_ASSERT_TRUE(enroll(*store, 3, 11, 2));
    Frame ack{};
    for (size_t i = 1; i <= QueueCapacity; ++i) {
        SCENARIO(i);
        receiveOk(*store, i);
    }
    EXPECT_RESULT(Result::Full, receive(binding(3, 2), data(1, 3, 2), *store, ack));
    TEST_ASSERT_EQUAL_UINT32(0, ack.size);
    Receipt receipt{};
    TEST_ASSERT_TRUE(store->load(binding(3, 2), receipt));
    TEST_ASSERT_EQUAL_UINT64(0, receipt.counter);
    EXPECT_RESULT(Result::Duplicate, receive(binding(), data(QueueCapacity), *store, ack));
    store = remount(records, Role::Receiver);
    TEST_ASSERT_TRUE(store->healthy());
    TEST_ASSERT_EQUAL_UINT32(QueueCapacity, store->queued());
    // Drain half, refill across the ring boundary, and check order after a restart.
    for (uint64_t counter = 1; counter <= 64; ++counter)
        EXPECT_RESULT(Result::Ok, store->forwarded(2, 10, counter));
    for (uint64_t counter = 1; counter <= 64; ++counter) receiveOk(*store, counter, 3, 2);
    TEST_ASSERT_EQUAL_UINT32(QueueCapacity, store->queued());
    store = remount(records, Role::Receiver);
    TEST_ASSERT_EQUAL_UINT32(QueueCapacity, store->queued());
    QueuedSample q{};
    TEST_ASSERT_TRUE(store->peek(q));
    TEST_ASSERT_EQUAL_UINT64(2, q.node);
    TEST_ASSERT_EQUAL_UINT64(65, q.counter);
    for (uint64_t counter = 65; counter <= QueueCapacity; ++counter)
        EXPECT_RESULT(Result::Ok, store->forwarded(2, 10, counter));
    TEST_ASSERT_TRUE(store->peek(q));
    TEST_ASSERT_EQUAL_UINT64(3, q.node);
    TEST_ASSERT_EQUAL_UINT64(1, q.counter);
}
void test_rotation_revocation_and_retired_keys_are_persistent() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    receiveOk(*store, 1);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 11));
    Receipt receipt{};
    TEST_ASSERT_FALSE(store->load(binding(), receipt));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 12, key(1), 1));
    EXPECT_RESULT(Result::Conflict, store->activate(2, 10));
    QueuedSample q{};
    TEST_ASSERT_TRUE(store->peek(q));
    TEST_ASSERT_EQUAL_UINT64(10, q.generation);
    store = remount(records, Role::Receiver);
    Binding b{};
    TEST_ASSERT_TRUE(store->binding(2, b));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key(2).data(), b.key.data(), 16);
    EXPECT_RESULT(Result::Ok, store->revoke(2, 11));
    EXPECT_RESULT(Result::Ok, store->revoke(2, 11));
    TEST_ASSERT_FALSE(store->binding(2, b));
    TEST_ASSERT_TRUE(store->peek(q)); // Queued samples of a revoked credential still decode.
}
void test_bad_provisioning_and_identity_do_not_mutate_state() {
    MemoryRecords records;
    auto store = mounted(records);
    EXPECT_RESULT(Result::Invalid, store->prepare(0, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 2, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 3, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 2, 0, key(), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 2, 10, key(0), 1));
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 1, 2, 10, key(), 2));
    TEST_ASSERT_EQUAL_UINT32(0, records.writes);
    TEST_ASSERT_TRUE(enroll(*store));
    EXPECT_RESULT(Result::Conflict, store->prepare(43, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 10, key(2), 1));
    EXPECT_RESULT(Result::NotFound, store->activate(2, 100));
    EXPECT_RESULT(Result::NotFound, store->revoke(2, 100));
    store = remount(records, Role::Receiver);
    EXPECT_HEALTH(Health::Role, *store);
}
void test_full_registry_reclaims_only_revoked_slots_without_samples() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 3, 10, key(2), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 10));
    for (size_t i = 1; i < BindingCapacity; ++i)
        TEST_ASSERT_TRUE(enroll(*store, i + 2, i + 10, uint8_t(i + 1)));
    TEST_ASSERT_EQUAL_size_t(0, store->freeSlots());
    EXPECT_RESULT(Result::Full, store->prepare(42, 1, 100, 100, key(100), 1));
    // A revoked slot with a queued sample stays until the sample is forwarded.
    receiveOk(*store, 1);
    EXPECT_RESULT(Result::Ok, store->revoke(2, 10));
    TEST_ASSERT_EQUAL_size_t(0, store->freeSlots());
    EXPECT_RESULT(Result::Full, store->prepare(42, 1, 100, 100, key(100), 1));
    EXPECT_RESULT(Result::Ok, store->forwarded(2, 10, 1));
    TEST_ASSERT_EQUAL_size_t(1, store->freeSlots());
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 100, 100, key(100), 1));
    EnrollmentInfo info{};
    TEST_ASSERT_FALSE(store->info(2, 10, info)); // Freed.
    // The freed credential can never return, in memory or after a restart.
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 999, key(), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 10, key(200), 1));
    store = remount(records, Role::Receiver);
    TEST_ASSERT_TRUE(store->healthy());
    TEST_ASSERT_TRUE(store->info(100, 100, info));
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 999, key(), 1));
    // The new occupant does not inherit the old receipt left in the slot's record.
    EXPECT_RESULT(Result::Ok, store->activate(100, 100));
    Receipt receipt{};
    TEST_ASSERT_TRUE(store->load(binding(100, 100), receipt));
    TEST_ASSERT_EQUAL_UINT64(0, receipt.counter);
    receiveOk(*store, 1, 100, 100);
}
void test_power_loss_while_freeing_a_slot_keeps_the_old_key_retired() {
    for (int step = 0; step < 2; ++step) {
        SCENARIO(step);
        MemoryRecords records;
        auto store = mounted(records);
        for (uint64_t generation = 1; generation <= BindingCapacity; ++generation) {
            EXPECT_RESULT(Result::Ok,
                          store->prepare(42, 1, 2, generation, key(uint8_t(generation)), 1));
            EXPECT_RESULT(Result::Ok, store->activate(2, generation));
        }
        records.failAt = step; // 0: retired list write lost; 1: registry write lost.
        EXPECT_RESULT(Result::StorageError, store->prepare(42, 1, 2, 50, key(50), 1));
        store = remount(records);
        TEST_ASSERT_TRUE(store->healthy()); // Revoked-and-retired is a valid intermediate.
        EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 50, key(50), 1));
        EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 51, key(1), 1));
    }
}
void test_retired_list_forgets_the_oldest_when_full() {
    records::RetiredList list{};
    for (uint64_t i = 1; i <= records::RetiredCapacity + 1; ++i) records::retire(list, i, i + 1000);
    TEST_ASSERT_EQUAL_size_t(records::RetiredCapacity, list.count);
    TEST_ASSERT_FALSE(records::isRetired(list, 1, 1));
    TEST_ASSERT_TRUE(records::isRetired(list, 2, 0));
    TEST_ASSERT_TRUE(records::isRetired(list, 0, records::RetiredCapacity + 1001));
    std::vector<uint8_t> bytes(records::RetiredRecordCapacity);
    const size_t size = records::encode(list, bytes.data());
    TEST_ASSERT_EQUAL_size_t(records::RetiredRecordCapacity, size);
    records::RetiredList decoded{};
    TEST_ASSERT_TRUE(records::decode(bytes.data(), size, decoded));
    TEST_ASSERT_EQUAL_size_t(list.count, decoded.count);
    bytes[2] = uint8_t(records::RetiredCapacity + 1);
    repairChecksum(bytes);
    TEST_ASSERT_FALSE(records::decode(bytes.data(), size, decoded));
    bytes.assign(bytes.begin(), bytes.begin() + 3 + 16 + 4);
    bytes[2] = 1;
    setNumber(bytes, 3, 0); // A zero generation is never retired.
    repairChecksum(bytes);
    TEST_ASSERT_FALSE(records::decode(bytes.data(), bytes.size(), decoded));
    uint64_t a = 0, b = 0;
    TEST_ASSERT_TRUE(records::fingerprint(key(1), a));
    TEST_ASSERT_TRUE(records::fingerprint(key(2), b));
    TEST_ASSERT_TRUE(a != b && a != 0);
}
void test_reset_leaves_the_network_and_retires_every_key() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(enroll(*store));
    uint64_t counter = 0;
    TEST_ASSERT_TRUE(store->reserve(binding(), counter));
    EXPECT_RESULT(Result::Ok, store->reset(false));
    TEST_ASSERT_EQUAL_UINT64(0, store->network());
    EnrollmentInfo list[BindingCapacity]{};
    TEST_ASSERT_EQUAL_size_t(0, store->list(list, BindingCapacity));
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    store = remount(records);
    TEST_ASSERT_TRUE(store->healthy());
    TEST_ASSERT_EQUAL_UINT64(0, store->network());
    // The old credential is refused in any network; fresh ones join another network.
    EXPECT_RESULT(Result::Conflict, store->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Conflict, store->prepare(77, 5, 2, 20, key(), 1));
    EXPECT_RESULT(Result::Ok, store->prepare(77, 5, 2, 20, key(9), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 20));
    TEST_ASSERT_EQUAL_UINT64(77, store->network());
    // A receiver with queued samples refuses unless they are discarded.
    MemoryRecords rxRecords;
    auto rx = mounted(rxRecords, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*rx));
    receiveOk(*rx, 1);
    EXPECT_RESULT(Result::Conflict, rx->reset(false));
    for (int step = 0; step < 4; ++step) { // Power loss after each write of the reset.
        SCENARIO(step);
        rxRecords.failAt = step;
        EXPECT_RESULT(Result::StorageError, rx->reset(true));
        rx = remount(rxRecords, Role::Receiver);
        TEST_ASSERT_TRUE(rx->healthy());
        Binding b{};
        if (step) TEST_ASSERT_FALSE(rx->binding(2, b)); // Revoked from the first write on.
        TEST_ASSERT_EQUAL_UINT32(step < 3 ? 1 : 0, rx->queued());
    }
    EXPECT_RESULT(Result::Ok, rx->reset(true));
    TEST_ASSERT_EQUAL_UINT32(0, rx->queued());
    rx = remount(rxRecords, Role::Receiver);
    TEST_ASSERT_EQUAL_UINT32(0, rx->queued());
    EXPECT_RESULT(Result::Ok, rx->prepare(42, 1, 2, 30, key(3), 1));
    EXPECT_RESULT(Result::Ok, rx->activate(2, 30));
    receiveOk(*rx, 1, 2, 3); // Sequences continue after the discarded queue.
    rx = remount(rxRecords, Role::Receiver);
    TEST_ASSERT_EQUAL_UINT32(1, rx->queued());
    records.failBefore = true;
    EXPECT_RESULT(Result::StorageError, store->reset(false));
    EXPECT_RESULT(Result::StorageError, store->reset(false));
}
void test_migrates_a_v1_snapshot_once() {
    for (int interrupted = 0; interrupted < 2; ++interrupted) {
        SCENARIO(interrupted);
        // Build a v1 device: two nodes, one rotated, three queued samples.
        std::unique_ptr<snapshot::State> v1(new snapshot::State());
        v1->revision = 9;
        v1->network = 42;
        v1->receiver = 1;
        v1->profile = 1;
        auto& a = v1->entries[0];
        a.state = Enrollment::Revoked;
        a.node = 2;
        a.generation = 10;
        a.key = key(1);
        auto& b = v1->entries[1];
        b.state = Enrollment::Active;
        b.node = 2;
        b.generation = 11;
        b.key = key(2);
        auto& c = v1->entries[2];
        c.state = Enrollment::Active;
        c.node = 3;
        c.generation = 12;
        c.key = key(3);
        a.receipt.counter = 4;
        a.receipt.last = data(4, 2, 1);
        b.receipt.counter = 7;
        b.receipt.last = data(7, 2, 2);
        c.receipt.counter = 1;
        c.receipt.last = data(1, 3, 3);
        v1->count = 3;
        v1->queue[0].entry = 0;
        v1->queue[0].frame = data(4, 2, 1);
        v1->queue[1].entry = 1;
        v1->queue[1].frame = data(7, 2, 2);
        v1->queue[2].entry = 2;
        v1->queue[2].frame = data(1, 3, 3);
        MemoryRecords records;
        std::vector<uint8_t> bytes(SnapshotSize);
        bytes.resize(snapshot::encode(*v1, Role::Receiver, 1, bytes.data()));
        records.records["snapshot"] = bytes;
        if (interrupted) {
            records.failAt = 3; // After the queue records, before the registry.
            auto store = remount(records, Role::Receiver);
            EXPECT_HEALTH(Health::WriteError, *store);
            TEST_ASSERT_FALSE(records.has("registry"));
            TEST_ASSERT_TRUE(records.has("snapshot"));
        }
        auto store = remount(records, Role::Receiver);
        TEST_ASSERT_TRUE(store->healthy());
        TEST_ASSERT_FALSE(records.has("snapshot"));
        TEST_ASSERT_EQUAL_UINT64(42, store->network());
        TEST_ASSERT_EQUAL_UINT32(3, store->queued());
        Frame ack{};
        EXPECT_RESULT(Result::Duplicate, receive(binding(2, 2), data(7, 2, 2), *store, ack));
        EXPECT_RESULT(Result::Replay, receive(binding(2, 2), data(6, 2, 2), *store, ack));
        EXPECT_RESULT(Result::Ok, receive(binding(3, 3), data(2, 3, 3), *store, ack));
        QueuedSample q{};
        const uint64_t order[][2] = {{10, 4}, {11, 7}, {12, 1}, {12, 2}};
        for (const auto& expected : order) {
            TEST_ASSERT_TRUE(store->peek(q));
            TEST_ASSERT_EQUAL_UINT64(expected[0], q.generation);
            TEST_ASSERT_EQUAL_UINT64(expected[1], q.counter);
            EXPECT_RESULT(Result::Ok, store->forwarded(q.node, q.generation, q.counter));
        }
        store = remount(records, Role::Receiver);
        TEST_ASSERT_TRUE(store->healthy());
        TEST_ASSERT_EQUAL_UINT32(0, store->queued());
    }
    // A transmitter keeps its counter; a damaged v1 snapshot is never migrated.
    std::unique_ptr<snapshot::State> tx(new snapshot::State());
    tx->revision = 3;
    tx->network = 42;
    tx->receiver = 1;
    tx->profile = 1;
    tx->entries[0].state = Enrollment::Active;
    tx->entries[0].node = 2;
    tx->entries[0].generation = 10;
    tx->entries[0].key = key();
    tx->entries[0].counter = 57;
    MemoryRecords records;
    std::vector<uint8_t> bytes(SnapshotSize);
    bytes.resize(snapshot::encode(*tx, Role::Transmitter, 2, bytes.data()));
    records.records["snapshot"] = bytes;
    auto store = remount(records);
    uint64_t counter = 0;
    TEST_ASSERT_TRUE(store->reserve(binding(), counter));
    TEST_ASSERT_EQUAL_UINT64(58, counter);
    MemoryRecords damaged;
    bytes[20] ^= 1;
    damaged.records["snapshot"] = bytes;
    store = remount(damaged);
    EXPECT_HEALTH(Health::Corrupt, *store);
    TEST_ASSERT_EQUAL_size_t(0, damaged.writes);
    damaged.failReadOf = "snapshot";
    store = remount(damaged);
    EXPECT_HEALTH(Health::ReadError, *store);
    MemoryRecords failing;
    failing.records["snapshot"] = std::vector<uint8_t>(12);
    failing.failBefore = true;
    store = remount(failing);
    EXPECT_HEALTH(Health::Corrupt, *store);
}
void test_semantically_invalid_registries_fail_closed_even_with_valid_crc() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(enroll(*store));
    uint64_t value = 0;
    TEST_ASSERT_TRUE(store->reserve(binding(), value));
    store.reset();
    const auto good = records.bytes("registry");
    struct Mutation {
        size_t offset;
        uint8_t value;
        Health health;
    };
    const Mutation invalid[] = {
        {0, 0, Health::Format},
        {1, 2, Health::Format},
        {RegRole, 2, Health::Role},
        {RegDevice, 3, Health::Device},
        {RegProfile, 2, Health::Invalid},
        {RegCount, 2, Health::Invalid},
        {RegCount, 17, Health::Invalid},
        {RegSlot, 16, Health::Invalid},
        {RegState, 0, Health::Invalid},
        {RegState, 4, Health::Invalid},
        {RegNode + 7, 3, Health::Invalid},
        {RegState, 1, Health::Invalid}, // Prepared, counter set.
    };
    for (const auto& mutation : invalid) {
        SCENARIO(mutation.offset);
        records.bytes("registry") = good;
        records.bytes("registry")[mutation.offset] = mutation.value;
        repairChecksum(records.bytes("registry"));
        store = remount(records);
        EXPECT_HEALTH(mutation.health, *store);
    }
    const size_t zeroed[][2] = {{RegRevision, 8},   {RegNetwork, 8}, {RegReceiver, 8},
                                {RegGeneration, 8}, {RegKey, 16},    {RegNode, 8}};
    for (const auto& field : zeroed) {
        SCENARIO(field[0]);
        records.bytes("registry") = good;
        for (size_t i = 0; i < field[1]; ++i) records.bytes("registry")[field[0] + i] = 0;
        repairChecksum(records.bytes("registry"));
        store = remount(records);
        EXPECT_HEALTH(Health::Invalid, *store);
    }
    records.bytes("registry") = good;
    records.bytes("registry").push_back(0); // Trailing byte.
    repairChecksum(records.bytes("registry"));
    store = remount(records);
    EXPECT_HEALTH(Health::Invalid, *store);
    records.bytes("registry").resize(12);
    store = remount(records);
    EXPECT_HEALTH(Health::Corrupt, *store);
    records.bytes("registry") = good;
    records.bytes("registry")[5] ^= 1;
    store = remount(records);
    EXPECT_HEALTH(Health::Corrupt, *store);
    records.bytes("registry") = good;
    records.records["retired"] = std::vector<uint8_t>(5);
    store = remount(records);
    EXPECT_HEALTH(Health::Corrupt, *store);
    records.failReadOf = "retired";
    store = remount(records);
    EXPECT_HEALTH(Health::ReadError, *store);
}
void test_duplicate_credentials_and_active_nodes_are_rejected() {
    MemoryRecords records;
    auto store = mounted(records);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 10, key(1), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 10));
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Ok, store->activate(2, 11));
    store.reset();
    const auto good = records.bytes("registry");
    const size_t second = RegSlot + RegEntry;
    for (int scenario = 0; scenario < 5; ++scenario) {
        SCENARIO(scenario);
        auto& bytes = records.bytes("registry");
        bytes = good;
        if (scenario == 0)
            std::copy(good.begin() + RegKey, good.begin() + RegKey + 16,
                      bytes.begin() + second + (RegKey - RegSlot));
        if (scenario == 1) setNumber(bytes, second + (RegGeneration - RegSlot), 10);
        if (scenario == 2) bytes[RegState] = uint8_t(Enrollment::Active); // Two active keys.
        if (scenario == 3) bytes[second] = 0;                             // Same slot twice.
        if (scenario == 4) { // Both prepared for one node.
            bytes[RegState] = uint8_t(Enrollment::Prepared);
            bytes[second + 1] = uint8_t(Enrollment::Prepared);
            setNumber(bytes, RegCounter, 0);
        }
        repairChecksum(bytes);
        store = remount(records);
        EXPECT_HEALTH(Health::Invalid, *store);
    }
    // An active credential that is also retired is corruption.
    records.bytes("registry") = good;
    store = remount(records);
    TEST_ASSERT_TRUE(store->healthy());
    records::RetiredList retired{};
    uint64_t print = 0;
    TEST_ASSERT_TRUE(records::fingerprint(key(2), print));
    records::retire(retired, 999, print);
    std::vector<uint8_t> bytes(records::RetiredRecordCapacity);
    bytes.resize(records::encode(retired, bytes.data()));
    records.records["retired"] = bytes;
    store = remount(records);
    EXPECT_HEALTH(Health::Invalid, *store);
}
void test_corrupt_receiver_receipts_and_queued_frames_are_rejected() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    receiveOk(*store, 1);
    store.reset();
    const auto goodReceipt = records.bytes("r00");
    const auto goodQueue = records.bytes("q00");
    const auto goodRegistry = records.bytes("registry");
    auto restore = [&] {
        records.records["r00"] = goodReceipt;
        records.records["q00"] = goodQueue;
        records.records["registry"] = goodRegistry;
    };
    struct Case {
        const char* name;
        size_t offset;
        uint8_t value;
        Health health;
    };
    const Case cases[] = {
        {"r00", ReceiptCounter + 7, 2, Health::Invalid},   // Counter differs from frame.
        {"r00", ReceiptFrame + 40, 0x55, Health::Invalid}, // Tampered frame.
        {"r00", ReceiptThrough + 7, 2, Health::Invalid},   // Claims a missing record.
        {"r00", 5, 0x55, Health::Corrupt},                 // CRC not repaired below.
        {"q00", QueueSequence + 7, 5, Health::Invalid},
        {"q00", QueueSlot, 3, Health::Invalid},
        {"q00", QueueGeneration + 7, 3, Health::Invalid},
        {"q00", QueueFrame + 40, 0x55, Health::Invalid},
        {"q00", QueueSlot, 16, Health::Corrupt},
        {"registry", RegState, uint8_t(Enrollment::Prepared), Health::Invalid},
    };
    for (const auto& c : cases) {
        UNITY_SET_DETAIL(c.name);
        restore();
        records.bytes(c.name)[c.offset] = c.value;
        if (c.health != Health::Corrupt || c.offset == QueueSlot)
            repairChecksum(records.bytes(c.name));
        store = remount(records, Role::Receiver);
        EXPECT_HEALTH(c.health, *store);
    }
    // A queued frame newer than its receipt, and a missing queue record.
    restore();
    records::QueueRecord later{};
    later.slot = 0;
    later.generation = 10;
    later.frame = data(2);
    std::vector<uint8_t> bytes(records::QueueRecordCapacity);
    bytes.resize(records::encode(later, bytes.data()));
    records.bytes("q00") = bytes;
    store = remount(records, Role::Receiver);
    EXPECT_HEALTH(Health::Invalid, *store);
    restore();
    records.records.erase("q00");
    store = remount(records, Role::Receiver);
    EXPECT_HEALTH(Health::Invalid, *store);
    restore();
    for (const char* name : {"r00", "q00", "head"}) {
        UNITY_SET_DETAIL(name);
        if (!records.has(name)) records.records[name] = std::vector<uint8_t>(3);
        records.failReadOf = name;
        store = remount(records, Role::Receiver);
        EXPECT_HEALTH(Health::ReadError, *store);
    }
    records.failReadOf.clear();
    records.bytes("head") = std::vector<uint8_t>(3);
    store = remount(records, Role::Receiver);
    EXPECT_HEALTH(Health::Corrupt, *store);
    records.records.erase("head");
    records.bytes("q00") = std::vector<uint8_t>(3);
    store = remount(records, Role::Receiver);
    EXPECT_HEALTH(Health::Corrupt, *store);
    // A receipt left by another generation in this slot is ignored, not trusted.
    restore();
    setNumber(records.bytes("r00"), 2, 77);
    repairChecksum(records.bytes("r00"));
    records.records.erase("q00");
    store = remount(records, Role::Receiver);
    TEST_ASSERT_TRUE(store->healthy());
    TEST_ASSERT_EQUAL_UINT32(0, store->queued());
    // The front record becoming unreadable after mount latches the store.
    restore();
    store = remount(records, Role::Receiver);
    QueuedSample q{};
    records.failReadOf = "q00";
    TEST_ASSERT_FALSE(store->peek(q));
    EXPECT_HEALTH(Health::ReadError, *store);
    EXPECT_RESULT(Result::StorageError, store->forwarded(2, 10, 1));
    records.failReadOf.clear();
    store = remount(records, Role::Receiver);
    records.bytes("q00")[QueueFrame + 40] ^= 1;
    TEST_ASSERT_FALSE(store->peek(q));
    EXPECT_HEALTH(Health::Corrupt, *store);
    store = remount(records, Role::Receiver);
    EXPECT_HEALTH(Health::Corrupt, *store);
}
void test_counter_revision_exhaustion_and_operation_guards() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(enroll(*store));
    store.reset();
    for (size_t i = RegCounter; i < RegCounter + 8; ++i) records.bytes("registry")[i] = 0xff;
    repairChecksum(records.bytes("registry"));
    store = remount(records);
    TEST_ASSERT_TRUE(store->healthy());
    uint64_t counter = 1;
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    TEST_ASSERT_EQUAL_UINT64(0, counter);
    store.reset();
    for (size_t i = RegRevision; i < RegRevision + 8; ++i) records.bytes("registry")[i] = 0xff;
    repairChecksum(records.bytes("registry"));
    store = remount(records);
    TEST_ASSERT_TRUE(store->healthy());
    EXPECT_RESULT(Result::StorageError, store->revoke(2, 10));
    EXPECT_RESULT(Result::StorageError, store->revoke(2, 10));
    EXPECT_RESULT(Result::StorageError, store->forwarded(2, 10, 1));
    Binding b{};
    EnrollmentInfo info{};
    TEST_ASSERT_FALSE(store->binding(2, b));
    TEST_ASSERT_FALSE(store->info(2, 10, info));
    TEST_ASSERT_EQUAL_size_t(0, store->freeSlots());
    MemoryRecords receiverRecords;
    store = mounted(receiverRecords, Role::Receiver);
    EXPECT_RESULT(Result::Invalid, store->prepare(42, 3, 2, 10, key(), 1));
    TEST_ASSERT_TRUE(enroll(*store));
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    Receipt receipt{};
    receipt.counter = 1;
    receipt.last = data(1);
    EXPECT_RESULT(Result::Conflict, store->commit(binding(), 1, receipt));
    receipt.counter = 2;
    EXPECT_RESULT(Result::Invalid, store->commit(binding(), 0, receipt));
    EXPECT_RESULT(Result::Unauthorized, store->commit(binding(3), 0, receipt));
    auto wrong = binding();
    wrong.active = false;
    TEST_ASSERT_FALSE(store->load(wrong, receipt));
    wrong = binding();
    wrong.network = 99;
    TEST_ASSERT_FALSE(store->load(wrong, receipt));
    EXPECT_RESULT(Result::Conflict, store->forwarded(2, 10, 1));
    MemoryRecords txRecords;
    auto tx = mounted(txRecords);
    TEST_ASSERT_TRUE(enroll(*tx));
    TEST_ASSERT_FALSE(tx->load(binding(), receipt));
    EXPECT_RESULT(Result::Unauthorized, tx->commit(binding(), 0, receipt));
}
void test_mount_reports_why_storage_is_unavailable() {
    MemoryRecords records;
    auto store = mounted(records);
    TEST_ASSERT_TRUE(enroll(*store));
    EXPECT_HEALTH(Health::Ready, *store);
    store = mounted(records, Role::Receiver);
    EXPECT_HEALTH(Health::Role, *store);
    store.reset(new PersistentStore(records, Role::Transmitter, 3));
    store->mount();
    EXPECT_HEALTH(Health::Device, *store);
    store.reset(new PersistentStore(records, Role(3), 2));
    EXPECT_HEALTH(Health::Unmounted, *store);
    store->mount();
    EXPECT_HEALTH(Health::Identity, *store);
    store.reset(new PersistentStore(records, Role::Transmitter, 0));
    store->mount();
    EXPECT_HEALTH(Health::Identity, *store);
    store = mounted(records);
    EXPECT_HEALTH(Health::Ready, *store);
    records.failAfter = true;
    uint64_t counter = 0;
    TEST_ASSERT_FALSE(store->reserve(binding(), counter));
    EXPECT_HEALTH(Health::WriteError, *store);
}
void test_commit_on_unhealthy_store_is_a_storage_error() {
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    records.failAfter = true;
    Frame ack{};
    EXPECT_RESULT(Result::StorageError, receive(binding(), data(1), *store, ack));
    Receipt receipt{};
    receipt.counter = 2;
    receipt.last = data(2);
    EXPECT_RESULT(Result::StorageError, store->commit(binding(), 0, receipt));
    EXPECT_RESULT(Result::StorageError, store->reset(true));
}
void test_record_codecs_reject_malformed_input() {
    records::ReceiptRecord receipt{};
    receipt.generation = 10;
    receipt.receipt.counter = 1;
    receipt.receipt.last = data(1);
    std::vector<uint8_t> bytes(records::ReceiptCapacity);
    bytes.resize(records::encode(receipt, bytes.data()));
    records::ReceiptRecord decoded{};
    TEST_ASSERT_TRUE(records::decode(bytes.data(), bytes.size(), decoded));
    TEST_ASSERT_TRUE(sameFrame(receipt.receipt.last, decoded.receipt.last));
    for (size_t cut : {size_t(1), size_t(40)}) {
        auto shorter = bytes;
        shorter.resize(bytes.size() - cut);
        repairChecksum(shorter);
        TEST_ASSERT_FALSE(records::decode(shorter.data(), shorter.size(), decoded));
    }
    auto oversized = bytes;
    setNumber(oversized, 26, MaxFrame + 1, 2);
    repairChecksum(oversized);
    TEST_ASSERT_FALSE(records::decode(oversized.data(), oversized.size(), decoded));
    auto kind = bytes;
    kind[0] = 'Q';
    repairChecksum(kind);
    TEST_ASSERT_FALSE(records::decode(kind.data(), kind.size(), decoded));
    TEST_ASSERT_FALSE(records::decode(nullptr, 0, decoded));
    uint64_t head = 0;
    TEST_ASSERT_FALSE(records::decodeHead(bytes.data(), bytes.size(), head));
    char name[records::NameCapacity];
    records::queueKey(QueueCapacity + 0x2a, name);
    TEST_ASSERT_EQUAL_STRING("q2a", name);
    records::receiptKey(15, name);
    TEST_ASSERT_EQUAL_STRING("r0f", name);
}
void test_snapshot_codec_round_trips_without_a_blob() {
    std::unique_ptr<snapshot::State> state(new snapshot::State());
    state->revision = 7;
    state->network = 42;
    state->receiver = 1;
    state->profile = 1;
    auto& entry = state->entries[0];
    entry.state = Enrollment::Active;
    entry.node = 2;
    entry.generation = 10;
    entry.key = key();
    entry.counter = 5;
    std::vector<uint8_t> bytes(SnapshotSize);
    const size_t size = snapshot::encode(*state, Role::Transmitter, 2, bytes.data());
    TEST_ASSERT_EQUAL_UINT(MinSnapshotSize + EntryRecordSize, size);
    std::unique_ptr<snapshot::State> decoded(new snapshot::State());
    EXPECT_RESULT(Health::Ready,
                  snapshot::decode(bytes.data(), size, Role::Transmitter, 2, *decoded));
    TEST_ASSERT_EQUAL_UINT64(7, decoded->revision);
    TEST_ASSERT_EQUAL_UINT64(5, decoded->entries[0].counter);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key().data(), decoded->entries[0].key.data(), KeySize);
    snapshot::reset(*decoded);
    EXPECT_RESULT(Health::Device,
                  snapshot::decode(bytes.data(), size, Role::Transmitter, 3, *decoded));
    EXPECT_RESULT(Health::Corrupt,
                  snapshot::decode(bytes.data(), size - 1, Role::Transmitter, 2, *decoded));
}
void test_repaired_node_keeps_its_used_key_until_it_sends_with_the_new_one() {
    for (int useNew = 0; useNew < 2; ++useNew) {
        SCENARIO(useNew);
        MemoryRecords records;
        auto store = mounted(records, Role::Receiver);
        TEST_ASSERT_TRUE(enroll(*store));
        receiveOk(*store, 1); // Generation 10 is in use.
        EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 11, key(2), 1));
        EXPECT_RESULT(Result::Ok, store->activateAlongside(2, 11));
        EXPECT_RESULT(Result::Ok, store->activateAlongside(2, 11));
        Binding both[3]{};
        TEST_ASSERT_EQUAL_size_t(2, store->bindings(2, both, 3));
        store = remount(records, Role::Receiver); // Two active generations are valid.
        TEST_ASSERT_TRUE(store->healthy());
        // The first new sample decides: the other generation is revoked durably.
        if (useNew)
            receiveOk(*store, 1, 2, 2);
        else
            receiveOk(*store, 2); // JOIN_DONE was lost: the node still uses the old key.
        EnrollmentInfo info{};
        TEST_ASSERT_TRUE(store->info(2, useNew ? 10 : 11, info));
        EXPECT_RESULT(Enrollment::Revoked, info.state);
        store = remount(records, Role::Receiver);
        TEST_ASSERT_EQUAL_size_t(1, store->bindings(2, both, 3));
        TEST_ASSERT_TRUE(both[0].key == key(useNew ? 2 : 1));
    }
    MemoryRecords records;
    auto store = mounted(records, Role::Receiver);
    TEST_ASSERT_TRUE(enroll(*store));
    // An unused previous generation is revoked at once; so is a second pending one.
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 11, key(2), 1));
    EXPECT_RESULT(Result::Ok, store->activateAlongside(2, 11));
    Binding both[2]{};
    TEST_ASSERT_EQUAL_size_t(1, store->bindings(2, both, 2));
    EXPECT_RESULT(Result::NotFound, store->activateAlongside(2, 99));
    EXPECT_RESULT(Result::Conflict, store->activateAlongside(2, 10));
    TEST_ASSERT_EQUAL_size_t(0, store->bindings(2, nullptr, 2));
    receiveOk(*store, 1, 2, 2);
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 2, 12, key(3), 1));
    EXPECT_RESULT(Result::Ok, store->activateAlongside(2, 12));
    records.failAt = 2; // Sample durable, revocation of the superseded key lost.
    Frame ack{};
    EXPECT_RESULT(Result::StorageError, receive(binding(2, 3), data(1, 2, 3), *store, ack));
    store = remount(records, Role::Receiver);
    EXPECT_RESULT(Result::Duplicate, receive(binding(2, 3), data(1, 2, 3), *store, ack));
    receiveOk(*store, 2, 2, 3); // The next sample completes the revocation.
    TEST_ASSERT_EQUAL_size_t(1, store->bindings(2, both, 2));
    MemoryRecords txRecords;
    auto tx = mounted(txRecords);
    EXPECT_RESULT(Result::Ok, tx->prepare(42, 1, 2, 10, key(), 1));
    EXPECT_RESULT(Result::Invalid, tx->activateAlongside(2, 10));
    EXPECT_RESULT(Result::Ok, store->prepare(42, 1, 5, 50, key(50), 1));
    records.failBefore = true;
    EXPECT_RESULT(Result::StorageError, store->activateAlongside(5, 50));
    EXPECT_RESULT(Result::StorageError, store->activateAlongside(5, 50));
}
// The v1 decoder now only feeds migration, but must still refuse anything invalid.
std::vector<uint8_t> v1Bytes(Role role) {
    std::unique_ptr<snapshot::State> state(new snapshot::State());
    state->revision = 3;
    state->network = 42;
    state->receiver = 1;
    state->profile = 1;
    auto& e = state->entries[0];
    e.state = Enrollment::Active;
    e.node = 2;
    e.generation = 10;
    e.key = key(1);
    if (role == Role::Transmitter) {
        e.counter = 1;
    } else {
        e.receipt.counter = 1;
        e.receipt.last = data(1);
        state->count = 1;
        state->queue[0].frame = data(1);
        auto& other = state->entries[1];
        other.state = Enrollment::Active;
        other.node = 3;
        other.generation = 11;
        other.key = key(2);
    }
    std::vector<uint8_t> bytes(SnapshotSize);
    bytes.resize(snapshot::encode(*state, role, role == Role::Transmitter ? 2 : 1, bytes.data()));
    return bytes;
}
Health decodeV1(const std::vector<uint8_t>& bytes, Role role) {
    std::unique_ptr<snapshot::State> state(new snapshot::State());
    return snapshot::decode(bytes.data(), bytes.size(), role, role == Role::Transmitter ? 2 : 1,
                            *state);
}
void test_v1_snapshot_decoder_still_fails_closed() {
    const auto tx = v1Bytes(Role::Transmitter);
    EXPECT_RESULT(Health::Ready, decodeV1(tx, Role::Transmitter));
    struct Mutation {
        size_t offset;
        uint8_t value;
    };
    const Mutation invalid[] = {{0, 0},  {4, 2},    {5, 2},  {13, 3},  {21, 0}, {29, 0}, {37, 0},
                                {39, 2}, {40, 255}, {41, 0}, {41, 17}, {42, 4}, {42, 0}, {42, 1},
                                {50, 0}, {50, 3},   {58, 0}, {91, 1},  {90, 1}, {92, 1}};
    for (const auto& mutation : invalid) {
        SCENARIO(mutation.offset);
        auto bytes = tx;
        bytes[mutation.offset] = mutation.value;
        repairChecksum(bytes);
        TEST_ASSERT_TRUE(decodeV1(bytes, Role::Transmitter) != Health::Ready);
    }
    auto bytes = tx;
    for (size_t i = 59; i < 75; ++i) bytes[i] = 0; // Zero key.
    repairChecksum(bytes);
    EXPECT_RESULT(Health::Invalid, decodeV1(bytes, Role::Transmitter));
    bytes = tx;
    bytes[20] ^= 1;
    EXPECT_RESULT(Health::Corrupt, decodeV1(bytes, Role::Transmitter));
    bytes.resize(12);
    EXPECT_RESULT(Health::Corrupt, decodeV1(bytes, Role::Transmitter));
    bytes.assign(SnapshotSize + 1, 0);
    EXPECT_RESULT(Health::Corrupt, decodeV1(bytes, Role::Transmitter));
    const auto rx = v1Bytes(Role::Receiver);
    EXPECT_RESULT(Health::Ready, decodeV1(rx, Role::Receiver));
    // Header receiver, tx-only counter, receipt, queue index/size and authenticated frame
    // (with the second enrollment the queue starts 186 bytes later than with one).
    const size_t flipped[] = {37, 82, 90, 92, 93, 228 + 186, 229 + 186, 231 + 186};
    for (size_t offset : flipped) {
        SCENARIO(offset);
        bytes = rx;
        bytes[offset] ^= 0xff;
        repairChecksum(bytes);
        TEST_ASSERT_TRUE(decodeV1(bytes, Role::Receiver) != Health::Ready);
    }
    const size_t second = SnapshotHeaderSize + EntryRecordSize;
    for (int scenario = 0; scenario < 4; ++scenario) {
        SCENARIO(scenario);
        bytes = rx;
        if (scenario == 0) std::copy(rx.begin() + 59, rx.begin() + 75, bytes.begin() + second + 17);
        if (scenario == 1) bytes[second + 16] = 10; // Same generation.
        if (scenario == 2) bytes[second + 8] = 2;   // Same node, both active.
        if (scenario == 3) bytes[228 + 186] = 1;    // Queued frame under the wrong entry.
        repairChecksum(bytes);
        EXPECT_RESULT(Health::Invalid, decodeV1(bytes, Role::Receiver));
    }
    bytes = rx;
    const auto later = data(2);
    std::copy(later.bytes.begin(), later.bytes.begin() + later.size, bytes.begin() + 231 + 186);
    repairChecksum(bytes);
    EXPECT_RESULT(Health::Invalid, decodeV1(bytes, Role::Receiver));
}
} // namespace
void runStorageTests() {
    UnitySetTestFile(__FILE__); // UNITY_BEGIN runs in test_main.cpp.
    RUN_TEST(test_durable_enrollment_and_counter_restart);
    RUN_TEST(test_uncertain_counter_write_cannot_reuse_nonce);
    RUN_TEST(test_failed_counter_write_and_corruption_fail_closed);
    RUN_TEST(test_receiver_commit_restart_duplicate_and_drain);
    RUN_TEST(test_each_sample_writes_two_small_records_whatever_the_state);
    RUN_TEST(test_power_loss_at_each_commit_step);
    RUN_TEST(test_ambiguous_head_write_repeats_at_most_one_sample);
    RUN_TEST(test_queue_capacity_wraps_and_survives_restart);
    RUN_TEST(test_rotation_revocation_and_retired_keys_are_persistent);
    RUN_TEST(test_bad_provisioning_and_identity_do_not_mutate_state);
    RUN_TEST(test_full_registry_reclaims_only_revoked_slots_without_samples);
    RUN_TEST(test_power_loss_while_freeing_a_slot_keeps_the_old_key_retired);
    RUN_TEST(test_retired_list_forgets_the_oldest_when_full);
    RUN_TEST(test_reset_leaves_the_network_and_retires_every_key);
    RUN_TEST(test_migrates_a_v1_snapshot_once);
    RUN_TEST(test_semantically_invalid_registries_fail_closed_even_with_valid_crc);
    RUN_TEST(test_duplicate_credentials_and_active_nodes_are_rejected);
    RUN_TEST(test_corrupt_receiver_receipts_and_queued_frames_are_rejected);
    RUN_TEST(test_counter_revision_exhaustion_and_operation_guards);
    RUN_TEST(test_mount_reports_why_storage_is_unavailable);
    RUN_TEST(test_commit_on_unhealthy_store_is_a_storage_error);
    RUN_TEST(test_record_codecs_reject_malformed_input);
    RUN_TEST(test_snapshot_codec_round_trips_without_a_blob);
    RUN_TEST(test_v1_snapshot_decoder_still_fails_closed);
    RUN_TEST(test_repaired_node_keeps_its_used_key_until_it_sends_with_the_new_one);
}
