#pragma once
#include "cajui_storage.h"

// Board-independent decisions of the radio images, kept out of the Arduino entry points
// so they are unit tested: which mode to boot into and how to recover from a fault.
namespace cajui {
// Left in RTC memory by the previous boot (USB console or button).
enum class BootRequest { None, Admin, Pair };
enum class BootMode { Admin, Pair, Run };
enum class AdminReason { None, Requested, Storage, NotEnrolled };
struct BootDecision {
    BootMode mode = BootMode::Admin;
    AdminReason reason = AdminReason::None;
};
// `mounted`: storage opened and validated. `pairHeld`: a transmitter's PRG was held at
// boot; the board reads it only when a pairing request is possible.
BootDecision decideBoot(const PersistentStore&, bool mounted, uint16_t radioProfile, BootRequest,
                        bool pairHeld);
const char* reasonName(AdminReason);

// A radio, storage or driver fault latches the application; the device then restarts
// after a delay that doubles with each consecutive fault, so a persistent fault neither
// busy-loops nor stays down until someone visits the site. A transmitter sleeps through
// the delay. The count is cleared after a healthy period or a completed delivery.
constexpr uint32_t FirstRetryMs = 10000, MaxRetryMs = 15UL * 60 * 1000;
constexpr uint32_t HealthyRunMs = 10UL * 60 * 1000;
uint32_t retryDelayMs(uint32_t consecutiveFaults);
} // namespace cajui
