// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef CAJUI_RUNTIME_ROLE
namespace board {
void initializePowerPins();
void sensorPower(bool enabled);
void prepareSleepPins(bool radioSleeping);
} // namespace board
#endif
