// SPDX-License-Identifier: Apache-2.0
// Pin assignments and hardware capabilities of the selected board, and the climate sensor
// choice. Each board is a directory under boards/ holding a board_profile.h; the build puts
// exactly one of them on the include path with -I, so a new board is a new directory and a
// new environment, with no edit to shared code. The sensor driver is selected separately
// with CAJUI_SENSOR_*.
//
// The board header is reached by a literal include through the include path, not through a
// macro (#include CAJUI_BOARD_PROFILE): PlatformIO's dependency scanner does not follow
// macro includes, so editing a pin did not recompile anything until a clean build.
#pragma once
#include <cstdint>

#if defined(CAJUI_SENSOR_SHT4X) && defined(CAJUI_SENSOR_DHT22)
#error Select exactly one climate sensor.
#endif
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1 && !defined(CAJUI_SENSOR_SHT4X) &&      \
    !defined(CAJUI_SENSOR_DHT22)
#error Select a climate sensor for the transmitter.
#endif

// #error does not stop the compiler; the include below then fails as a fatal error, so a
// build without a board ends with these two messages instead of one per board name used.
#if !__has_include("board_profile.h")
#error Add -Isrc/board/boards/<board> to the build_flags of the environment.
#endif
namespace board {
constexpr uint8_t NoPin = 255;
} // namespace board
#include "board_profile.h"

namespace board {
// Every board header defines these names; a missing one fails here, not inside a driver.
namespace contract {
constexpr uint8_t Pins[] = {Vext,       Led,          RadioCs,        RadioClock, RadioMiso,
                            RadioMosi,  RadioReset,   RadioBusy,      RadioDio,   RadioRfSwitch,
                            PairButton, BatterySense, BatteryControl, SensorSda,  SensorScl,
                            SensorData, OledSda,      OledScl,        OledReset};
constexpr bool Flags[] = {PairButtonExternalPullup, HasDisplay, HasBatterySense};
static_assert(BoardName[0] != '\0', "A board profile needs a name");
} // namespace contract
constexpr float TcxoVoltage = 1.8f;
constexpr uint8_t SensorAddress = 0x44;
#ifdef CAJUI_SENSOR_SHT4X
static_assert(SensorSda != NoPin && SensorScl != NoPin, "SHT4x requires I2C pins");
constexpr const char* SensorName = "sht4x";
#elif defined(CAJUI_SENSOR_DHT22)
static_assert(SensorData != NoPin, "DHT22 requires a data pin");
constexpr const char* SensorName = "dht22";
#endif
} // namespace board
