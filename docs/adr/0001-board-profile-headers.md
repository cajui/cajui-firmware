# ADR 0001: One profile directory per board

- Status: Accepted
- Date: 2026-11-01

## Context

Board support started as a single `src/board/profile.h` with nested `#ifdef` blocks: the
Heltec WiFi LoRa 32 V3 as the default, the Wireless Stick Lite inside it, and the XIAO
ESP32-S3 (#32) as a third branch around both. Every new board edited the same shared file
and deepened the nesting. Board facts had also spread outside the profile: the OLED pins
lived in `display.h`, the transmitter application chose its deep-sleep wake source from the
profile constant `PairButtonExternalPullup`, and the two climate drivers shared one file
through interleaved `#ifdef`s.

## Decision

- Each board is a directory under `src/board/boards/` holding a `board_profile.h` with every
  pin and capability of that board. The environment puts exactly one directory on the
  include path with `-Isrc/board/boards/<board>`; `profile.h` includes `"board_profile.h"`,
  checks that the full set of names is defined and adds the sensor constants. There is no
  default board: without the flag the build stops at the profile with a single fatal error
  naming `board_profile.h`.
- A limit of one board, such as the XIAO profile supporting transmitters only, is an
  `#error` in that board's header, not in shared code.
- Each climate sensor driver is one file under `src/board/sensors/`, selected by
  `CAJUI_SENSOR_*`, with one guard at the top of the file.
- Decisions that depend on a board capability stay in `src/board/`: `power.cpp` sets up and
  releases the pairing-button wake source. The applications use the profile's named pins
  but never branch on the board.
- A new board is a directory, a `platformio.ini` environment, an entry in the CI build
  (`.github/workflows/ci.yml`) and in `scripts/lint_board.py`, and a case in
  `tests_python/test_board_power.py`.

## Alternatives

- Keep `CAJUI_BOARD_*` flags and the single header: every board edits shared code and the
  nesting grows with each one.
- One header per board named by a macro (`#include CAJUI_BOARD_PROFILE`): no shared edit
  either, and it was the first version of this change, but PlatformIO's dependency scanner
  does not follow macro includes. Changing a pin in a board header and running `pio run`
  recompiled nothing until a clean build; only clean CI and release builds were safe.
- A `constexpr` struct per board: a stronger contract, but every consumer changes from
  `board::Name` to a struct field; not worth it for three boards of constants.
- `build_src_filter` to select role and sensor files instead of file guards: the selection
  leaves the code and the native fixture would repeat the same lists. The guards stay, one
  per file.

## Consequences

- Adding a board no longer touches `profile.h`, `display.h` or the applications, and
  editing a board header rebuilds the files that use it.
- A profile missing a name fails at `profile.h` with that name in the message; the native
  tests cover it with an incomplete fixture profile.
- Every runtime environment must name a board directory; the native board tests cover the
  four transmitter combinations and the rejected ones.
- The change is meant to keep behaviour: the same pins, holds, wake sources and sensor
  sequences, moved. That was checked by comparison with the previous code, the five
  builds and the native tests, not yet on hardware.
