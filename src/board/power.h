// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef CAJUI_RUNTIME_ROLE
namespace board {
void initializePowerPins();
void sensorPower(bool enabled);
void prepareSleepPins(bool radioSleeping);
// The pairing button as a deep-sleep wake source, with the pull-up the board needs.
void enablePairButtonWake();
// Returns the pairing button to a digital input after a wake from deep sleep.
void releasePairButtonWake();
} // namespace board
#endif
