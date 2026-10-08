// SPDX-License-Identifier: Apache-2.0
#include "cajui_service.h"
#include <algorithm>

namespace cajui {
RadioIdleGuard::RadioIdleGuard(RadioIdleAccess& access, uint32_t timeoutMs, uint32_t pollMs)
    : access_(access) {
    if (!timeoutMs || !pollMs) return;
    const uint32_t start = access_.nowMs();
    for (;;) {
        const uint32_t elapsed = access_.nowMs() - start;
        if (elapsed >= timeoutMs || !access_.take(timeoutMs - elapsed)) return;
        const uint32_t lockedAt = access_.nowMs() - start;
        if (lockedAt < timeoutMs && access_.idle()) {
            held_ = true;
            return;
        }
        access_.give();
        if (lockedAt >= timeoutMs) return;
        access_.wait(std::min(pollMs, timeoutMs - lockedAt));
    }
}
RadioIdleGuard::~RadioIdleGuard() {
    if (held_) access_.give();
}
bool startWatchedTask(WatchedTaskStartup& task) {
    if (!task.createParked()) return false;
    if (!task.subscribeWatchdog()) {
        task.discard();
        return false;
    }
    task.release();
    return true;
}
} // namespace cajui
