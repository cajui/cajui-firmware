// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include "cajui_service.h"
#include <vector>

namespace {
class IdleAccess final : public cajui::RadioIdleAccess {
public:
    uint32_t now = 0, lockDelay = 0, readyAt = 0;
    unsigned takes = 0, gives = 0, waits = 0;
    bool locked = false, available = true, listening = false;
    std::vector<uint32_t> budgets;
    uint32_t nowMs() const override { return now; }
    bool take(uint32_t timeoutMs) override {
        TEST_ASSERT_FALSE(locked);
        ++takes;
        budgets.push_back(timeoutMs);
        now += lockDelay;
        locked = available;
        return locked;
    }
    void give() override {
        TEST_ASSERT_TRUE(locked);
        locked = false;
        ++gives;
    }
    bool idle() const override {
        TEST_ASSERT_TRUE(locked);
        return listening;
    }
    void wait(uint32_t durationMs) override {
        TEST_ASSERT_FALSE(locked);
        TEST_ASSERT_GREATER_THAN_UINT32(0, durationMs);
        now += durationMs;
        ++waits;
        if (readyAt && now >= readyAt) listening = true;
    }
};

void test_idle_guard_holds_the_lock_until_the_operation_finishes() {
    IdleAccess access;
    access.listening = true;
    {
        cajui::RadioIdleGuard guard(access);
        TEST_ASSERT_TRUE(guard.ready());
        TEST_ASSERT_TRUE(access.locked);
        TEST_ASSERT_EQUAL_UINT32(0, access.gives);
    }
    TEST_ASSERT_FALSE(access.locked);
    TEST_ASSERT_EQUAL_UINT32(1, access.takes);
    TEST_ASSERT_EQUAL_UINT32(1, access.gives);
    TEST_ASSERT_EQUAL_UINT32(0, access.waits);
}
void test_busy_radio_times_out_without_granting_mutation_access() {
    IdleAccess access;
    {
        cajui::RadioIdleGuard guard(access, 12, 5);
        TEST_ASSERT_FALSE(guard.ready());
        TEST_ASSERT_FALSE(access.locked);
    }
    TEST_ASSERT_EQUAL_UINT32(12, access.now);
    TEST_ASSERT_EQUAL_UINT32(3, access.takes);
    TEST_ASSERT_EQUAL_UINT32(access.takes, access.gives);
    TEST_ASSERT_EQUAL_UINT32(12, access.budgets[0]);
    TEST_ASSERT_EQUAL_UINT32(7, access.budgets[1]);
    TEST_ASSERT_EQUAL_UINT32(2, access.budgets[2]);
    access.listening = true;
    cajui::RadioIdleGuard retry(access);
    TEST_ASSERT_TRUE(retry.ready());
}
void test_ack_completion_releases_waiter_before_deadline() {
    IdleAccess access;
    access.readyAt = 10;
    {
        cajui::RadioIdleGuard guard(access, 12, 5);
        TEST_ASSERT_TRUE(guard.ready());
        TEST_ASSERT_EQUAL_UINT32(10, access.now);
        TEST_ASSERT_TRUE(access.locked);
    }
    TEST_ASSERT_EQUAL_UINT32(access.takes, access.gives);
}
void test_deadline_includes_lock_contention_and_refuses_late_idle() {
    for (uint32_t delay : {12u, 13u}) {
        IdleAccess access;
        access.listening = true;
        access.lockDelay = delay;
        cajui::RadioIdleGuard guard(access, 12, 5);
        TEST_ASSERT_FALSE(guard.ready());
        TEST_ASSERT_FALSE(access.locked);
        TEST_ASSERT_EQUAL_UINT32(1, access.gives);
    }
    IdleAccess access;
    access.available = false;
    cajui::RadioIdleGuard guard(access);
    TEST_ASSERT_FALSE(guard.ready());
    TEST_ASSERT_EQUAL_UINT32(0, access.gives);
    TEST_ASSERT_EQUAL_UINT32(0, access.waits);
}
void test_busy_at_deadline_and_clock_rollover_fail_closed() {
    IdleAccess access;
    access.readyAt = 12;
    {
        cajui::RadioIdleGuard guard(access, 12, 5);
        TEST_ASSERT_FALSE(guard.ready());
    }
    access.now = UINT32_MAX - 5;
    access.readyAt = 0;
    access.listening = false;
    {
        cajui::RadioIdleGuard guard(access, 12, 5);
        TEST_ASSERT_FALSE(guard.ready());
    }
    TEST_ASSERT_EQUAL_UINT32(6, access.now);
    TEST_ASSERT_FALSE(access.locked);
}
void test_invalid_wait_policy_does_not_touch_the_lock() {
    IdleAccess access;
    cajui::RadioIdleGuard noTimeout(access, 0, 5);
    cajui::RadioIdleGuard noPoll(access, 12, 0);
    TEST_ASSERT_FALSE(noTimeout.ready());
    TEST_ASSERT_FALSE(noPoll.ready());
    TEST_ASSERT_EQUAL_UINT32(0, access.takes);
}

class TaskStartup final : public cajui::WatchedTaskStartup {
public:
    bool canCreate = true, canSubscribe = true, created = false, watched = false, running = false;
    unsigned creates = 0, subscriptions = 0, discards = 0, releases = 0;
    bool createParked() override {
        TEST_ASSERT_FALSE(created);
        ++creates;
        created = canCreate;
        return created;
    }
    bool subscribeWatchdog() override {
        TEST_ASSERT_TRUE(created);
        TEST_ASSERT_FALSE(running);
        ++subscriptions;
        watched = canSubscribe;
        return watched;
    }
    void release() override {
        TEST_ASSERT_TRUE(created);
        TEST_ASSERT_TRUE(watched);
        ++releases;
        running = true;
    }
    void discard() override {
        TEST_ASSERT_TRUE(created);
        TEST_ASSERT_FALSE(running);
        TEST_ASSERT_FALSE(watched);
        ++discards;
        created = false;
    }
};
void test_task_runs_only_after_watchdog_registration() {
    TaskStartup task;
    TEST_ASSERT_TRUE(cajui::startWatchedTask(task));
    TEST_ASSERT_TRUE(task.running);
    TEST_ASSERT_EQUAL_UINT32(1, task.creates);
    TEST_ASSERT_EQUAL_UINT32(1, task.subscriptions);
    TEST_ASSERT_EQUAL_UINT32(1, task.releases);
    TEST_ASSERT_EQUAL_UINT32(0, task.discards);
}
void test_task_creation_failure_never_registers_or_starts() {
    TaskStartup task;
    task.canCreate = false;
    TEST_ASSERT_FALSE(cajui::startWatchedTask(task));
    TEST_ASSERT_FALSE(task.running);
    TEST_ASSERT_EQUAL_UINT32(0, task.subscriptions);
    TEST_ASSERT_EQUAL_UINT32(0, task.discards);
}
void test_watchdog_failure_discards_parked_task_and_allows_clean_retry() {
    TaskStartup task;
    task.canSubscribe = false;
    TEST_ASSERT_FALSE(cajui::startWatchedTask(task));
    TEST_ASSERT_FALSE(task.created);
    TEST_ASSERT_FALSE(task.running);
    TEST_ASSERT_EQUAL_UINT32(1, task.discards);
    TEST_ASSERT_EQUAL_UINT32(0, task.releases);
    task.canSubscribe = true;
    TEST_ASSERT_TRUE(cajui::startWatchedTask(task));
    TEST_ASSERT_TRUE(task.running);
    TEST_ASSERT_EQUAL_UINT32(2, task.creates);
    TEST_ASSERT_EQUAL_UINT32(2, task.subscriptions);
}
} // namespace

void runServiceTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_idle_guard_holds_the_lock_until_the_operation_finishes);
    RUN_TEST(test_busy_radio_times_out_without_granting_mutation_access);
    RUN_TEST(test_ack_completion_releases_waiter_before_deadline);
    RUN_TEST(test_deadline_includes_lock_contention_and_refuses_late_idle);
    RUN_TEST(test_busy_at_deadline_and_clock_rollover_fail_closed);
    RUN_TEST(test_invalid_wait_policy_does_not_touch_the_lock);
    RUN_TEST(test_task_runs_only_after_watchdog_registration);
    RUN_TEST(test_task_creation_failure_never_registers_or_starts);
    RUN_TEST(test_watchdog_failure_discards_parked_task_and_allows_clean_retry);
}
