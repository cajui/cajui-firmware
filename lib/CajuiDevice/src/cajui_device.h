// SPDX-License-Identifier: Apache-2.0
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

// Transmit power. The configured value, set over USB, is the ceiling: the operator keeps
// it within what the region and antenna allow. The SX1262 high-power amplifier covers
// -9 to +22 dBm; without a stored value the bench default applies.
constexpr int8_t MinPowerDbm = -9, MaxPowerDbm = 22, DefaultPowerDbm = -9;
bool validPower(int dbm);
// Radio settings record v1: kind 'P', version 1, power (int8), CRC32.
constexpr size_t RadioRecordSize = 1 + 1 + 1 + 4;
ReadResult loadPower(AtomicBlob&, int8_t& dbm);
bool savePower(AtomicBlob&, int8_t dbm);
// Power a transmitter uses across cycles. A receiver's command (v2 ACK) moves it within
// [MinPowerDbm, configured]; after MissedAckLimit cycles without an ACK the node returns
// to the configured power, so a wrong command can never leave it unheard.
constexpr uint8_t MissedAckLimit = 3;
struct PowerState {
    int8_t dbm = DefaultPowerDbm;
    uint8_t missed = 0;
    int8_t ceiling = DefaultPowerDbm; // Configured power when the state was taken.
};
// The power to use now, from a remembered state (if any) and the configured ceiling. A
// state taken under another configured power is ignored: a new USB setting applies at the
// next restart instead of being held back by an old command.
int8_t currentPower(const PowerState*, int8_t configured);
PowerState nextPower(const PowerState& now, int8_t configured, bool acknowledged, int8_t command);
} // namespace cajui
