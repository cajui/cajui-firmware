// SPDX-License-Identifier: Apache-2.0
#include "cajui_setup_save.h"

namespace cajui {
void SetupSaveRetry::reset() {
    result_ = SetupSaveResult::None;
    attempts_ = 0;
    pending_ = false;
}
void SetupSaveRetry::record(SetupSaveResult result, uint32_t now) {
    result_ = result;
    if (attempts_ < SetupSaveAttempts) ++attempts_;
    pending_ = result == SetupSaveResult::RadioBusy && attempts_ < SetupSaveAttempts;
    retryAt_ = now + SetupSaveRetryMs;
}
bool SetupSaveRetry::due(uint32_t now) const {
    return pending_ && int32_t(now - retryAt_) >= 0;
}
} // namespace cajui
