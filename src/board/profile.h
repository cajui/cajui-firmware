// SPDX-License-Identifier: Apache-2.0
// Pin assignments and hardware capabilities of the selected board. Each board is a directory
// under boards/ holding a board_profile.h; the build puts exactly one of them on the include
// path with -I, so a new board is a new directory and a new environment, with no edit to
// shared code. Sensors are not chosen here or at build time: every driver is built in, the
// Sensor* pins say which ones the board can carry, and the transmitter detects what is
// attached (sensors/sensors.h).
//
// The board header is reached by a literal include through the include path, not through a
// macro (#include CAJUI_BOARD_PROFILE): PlatformIO's dependency scanner does not follow
// macro includes, so editing a pin did not recompile anything until a clean build.
#pragma once
#include <cstdint>

namespace board {
constexpr uint8_t NoPin = 255;
} // namespace board
// Without a board directory this include is the single, fatal error, and compilers print this
// line with it. An #error guarded by __has_include was tried: #error does not stop the build,
// and after __has_include fails GCC 13 skips the include, so one error per board name followed.
#include "board_profile.h" // From -Isrc/board/boards/<board> in the build_flags.

namespace board {
// Every board header defines these names; a missing one fails here, not inside a driver.
namespace contract {
constexpr uint8_t Pins[] = {Vext,       Led,          RadioCs,        RadioClock, RadioMiso,
                            RadioMosi,  RadioReset,   RadioBusy,      RadioDio,   RadioRfSwitch,
                            PairButton, BatterySense, BatteryControl, SensorSda,  SensorScl,
                            SensorData, SensorAnalog, OledSda,        OledScl,    OledReset};
constexpr bool Flags[] = {PairButtonExternalPullup, HasDisplay, HasBatterySense};
static_assert(BoardName[0] != '\0', "A board profile needs a name");
} // namespace contract
constexpr float TcxoVoltage = 1.8f;
constexpr uint8_t SensorAddress = 0x44;
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
static_assert((SensorSda != NoPin && SensorScl != NoPin) || SensorData != NoPin ||
                  SensorAnalog != NoPin,
              "A transmitter board needs at least one sensor pin");
#endif
static_assert((SensorSda == NoPin) == (SensorScl == NoPin), "I2C needs both sensor pins");
} // namespace board
