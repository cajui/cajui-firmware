// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace board {
// Reads once and releases the bus, then switches sensor power off where available.
// Invalid values are NaN and become sensor-error telemetry in climateSample().
void readClimate(float& temperature, float& humidity);
// Safe before initialization and before any deep sleep (also the critical-battery path).
void climateOff();
} // namespace board
