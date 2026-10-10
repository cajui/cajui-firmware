// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace cajui {
enum class SetupSaveResult : uint8_t {
    None,
    Saved,
    RadioBusy,
    RadioStopped,
    StorageFailed,
    ApplyFailed,
    Invalid
};
constexpr uint8_t SetupSaveAttempts = 3;
constexpr uint32_t SetupSaveRetryMs = 1000;
class SetupSaveRetry {
public:
    void reset();
    void record(SetupSaveResult, uint32_t now);
    bool due(uint32_t now) const;
    bool pending() const { return pending_; }
    SetupSaveResult result() const { return result_; }

private:
    SetupSaveResult result_ = SetupSaveResult::None;
    uint8_t attempts_ = 0;
    uint32_t retryAt_ = 0;
    bool pending_ = false;
};
} // namespace cajui
