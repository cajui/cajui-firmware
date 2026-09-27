// SPDX-License-Identifier: Apache-2.0
#pragma once
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include <cstdint>

namespace board {
// Battery voltage in millivolts from the board's divider, or 0 when the reading cannot be a
// single cell (no battery, or a wrong reading): unknown, never zero volts.
uint16_t readBatteryMv();
} // namespace board
#endif
