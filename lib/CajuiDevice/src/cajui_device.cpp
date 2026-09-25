// SPDX-License-Identifier: Apache-2.0
#include "cajui_device.h"
#include "cajui_crc32.h"

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
namespace {
constexpr uint8_t PowerKind = 'P', PowerVersion = 1;
int8_t clampPower(int dbm, int8_t ceiling) {
    return int8_t(dbm < MinPowerDbm ? MinPowerDbm : dbm > ceiling ? ceiling : dbm);
}
} // namespace
bool validPower(int dbm) {
    return dbm >= MinPowerDbm && dbm <= MaxPowerDbm;
}
ReadResult loadPower(AtomicBlob& blob, int8_t& dbm) {
    dbm = DefaultPowerDbm;
    uint8_t bytes[RadioRecordSize]{};
    size_t size = 0;
    const ReadResult read = blob.read(bytes, sizeof(bytes), size);
    if (read != ReadResult::Ok) return read;
    uint32_t crc = 0;
    for (size_t i = 3; i < RadioRecordSize; ++i) crc = (crc << 8) | bytes[i];
    // The stored byte is a two's-complement dBm value.
    const int value = bytes[2] < 0x80 ? int(bytes[2]) : int(bytes[2]) - 0x100;
    if (size != RadioRecordSize || bytes[0] != PowerKind || bytes[1] != PowerVersion ||
        crc != crc32(bytes, 3) || !validPower(value))
        return ReadResult::Error;
    dbm = int8_t(value);
    return ReadResult::Ok;
}
bool savePower(AtomicBlob& blob, int8_t dbm) {
    if (!validPower(dbm)) return false;
    uint8_t bytes[RadioRecordSize] = {PowerKind, PowerVersion, uint8_t(dbm)};
    const uint32_t crc = crc32(bytes, 3);
    for (int i = 0; i < 4; ++i) bytes[3 + i] = uint8_t(crc >> (8 * (3 - i)));
    return blob.replace(bytes, sizeof(bytes));
}
int8_t currentPower(const PowerState* state, int8_t configured) {
    const int8_t ceiling = validPower(configured) ? configured : DefaultPowerDbm;
    return state && state->ceiling == ceiling ? clampPower(state->dbm, ceiling) : ceiling;
}
PowerState nextPower(const PowerState& now, int8_t configured, bool acknowledged, int8_t command) {
    const int8_t ceiling = validPower(configured) ? configured : DefaultPowerDbm;
    PowerState next{};
    next.ceiling = ceiling;
    next.dbm = now.ceiling == ceiling ? clampPower(now.dbm, ceiling) : ceiling;
    next.missed = now.ceiling == ceiling ? now.missed : 0;
    const uint8_t missed = next.missed;
    if (!acknowledged) {
        next.missed = missed < MissedAckLimit ? uint8_t(missed + 1) : MissedAckLimit;
        if (next.missed >= MissedAckLimit) next.dbm = ceiling;
        return next;
    }
    next.missed = 0;
    if (command != KeepPower) next.dbm = clampPower(command, ceiling);
    return next;
}
uint32_t retryDelayMs(uint32_t consecutiveFaults) {
    uint32_t delay = FirstRetryMs;
    for (uint32_t fault = 1; fault < consecutiveFaults && delay < MaxRetryMs; ++fault) delay *= 2;
    return delay < MaxRetryMs ? delay : MaxRetryMs;
}
} // namespace cajui
