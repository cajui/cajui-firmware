// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace board {
// Powers the sensor for one reading and always powers it down, including on failure.
// Invalid values are NaN and become sensor-error telemetry in climateSample().
void readClimate(float& temperature, float& humidity);
// Safe before initialization and before any deep sleep (also the critical-battery path).
void climateOff();
} // namespace board
