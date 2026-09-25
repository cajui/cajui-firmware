// SPDX-License-Identifier: Apache-2.0
#include "cajui_device.h"

namespace cajui {
BootDecision decideBoot(const PersistentStore& store, bool mounted, uint16_t radioProfile,
                        BootRequest request, bool pairHeld) {
    BootDecision decision{};
    if (request == BootRequest::Admin) {
        decision.reason = AdminReason::Requested;
        return decision;
    }
    if (!mounted) {
        decision.reason = AdminReason::Storage;
        return decision;
    }
    const bool profileOk = store.profile() == radioProfile;
    if (store.role() == Role::Transmitter) {
        // Pairing needs only healthy storage: it is how an unenrolled node joins.
        if (request == BootRequest::Pair || pairHeld) {
            decision.mode = BootMode::Pair;
            return decision;
        }
        Binding binding{};
        if (store.network() && profileOk && store.binding(store.device(), binding)) {
            decision.mode = BootMode::Run;
            return decision;
        }
        decision.reason = AdminReason::NotEnrolled;
        return decision;
    }
    // A receiver runs without bindings so radio pairing can create the first one.
    if (!store.network() || profileOk) {
        decision.mode = BootMode::Run;
        return decision;
    }
    decision.reason = AdminReason::NotEnrolled;
    return decision;
}
const char* reasonName(AdminReason reason) {
    switch (reason) {
    case AdminReason::Requested: return "requested";
    case AdminReason::Storage: return "storage";
    case AdminReason::NotEnrolled: return "not_enrolled";
    case AdminReason::None: break;
    }
    return "none";
}
uint32_t retryDelayMs(uint32_t consecutiveFaults) {
    uint32_t delay = FirstRetryMs;
    for (uint32_t fault = 1; fault < consecutiveFaults && delay < MaxRetryMs; ++fault) delay *= 2;
    return delay < MaxRetryMs ? delay : MaxRetryMs;
}
} // namespace cajui
