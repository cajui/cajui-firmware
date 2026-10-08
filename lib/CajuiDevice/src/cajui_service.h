// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace cajui {
constexpr uint32_t RadioIdleWaitMs = 3000, RadioIdlePollMs = 5;

enum class RadioAccessState { Busy, Listening, Stopped };

class RadioIdleAccess {
public:
    virtual ~RadioIdleAccess() = default;
    virtual uint32_t nowMs() const = 0;
    virtual bool take(uint32_t timeoutMs) = 0;
    virtual void give() = 0;
    virtual RadioAccessState state() const = 0;
    virtual void wait(uint32_t durationMs) = 0;
};

class RadioIdleGuard {
public:
    explicit RadioIdleGuard(RadioIdleAccess&, uint32_t timeoutMs = RadioIdleWaitMs,
                            uint32_t pollMs = RadioIdlePollMs);
    ~RadioIdleGuard();
    RadioIdleGuard(const RadioIdleGuard&) = delete;
    RadioIdleGuard& operator=(const RadioIdleGuard&) = delete;
    bool ready() const { return held_; }
    RadioAccessState state() const { return state_; }

private:
    RadioIdleAccess& access_;
    bool held_ = false;
    RadioAccessState state_ = RadioAccessState::Busy;
};

class WatchedTaskStartup {
public:
    virtual ~WatchedTaskStartup() = default;
    virtual bool createParked() = 0;
    virtual bool subscribeWatchdog() = 0;
    virtual void release() = 0;
    virtual void discard() = 0;
};
bool startWatchedTask(WatchedTaskStartup&);
} // namespace cajui
