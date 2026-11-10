# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests_python/fixtures/board"
BOARDS = ROOT / "src/board/boards"
HELTEC = BOARDS / "heltec_wifi_lora_32_v3"
STICK_LITE = BOARDS / "heltec_wireless_stick_lite_v3"
XIAO = BOARDS / "xiao_esp32s3_wio_sx1262"
INCOMPLETE = FIXTURES / "incomplete_profile"
NO_SENSOR = FIXTURES / "no_sensor_profile"


class BoardPowerTests(unittest.TestCase):
    def compile(self, output, definitions, board=None, *, lifecycle=True):
        sources = [
            ROOT / "src/board/power.cpp",
            ROOT / "src/board/battery.cpp",
            FIXTURES / "lifecycle.cpp",
        ]
        command = [
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{FIXTURES}",
            f"-I{ROOT / 'src'}",
            *([f"-I{board}"] if board else []),
            *[f"-D{definition}" for definition in definitions],
        ]
        if lifecycle:
            command += [*[str(path) for path in sources], "-o", str(output)]
            return subprocess.run(command, capture_output=True, text=True, check=False)
        # A file that uses the board names, so a profile that is missing or incomplete shows
        # whether the build stops at profile.h or reports every name a driver uses.
        command += ["-fsyntax-only", str(ROOT / "src/board/power.cpp")]
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def test_sleep_wake_and_battery_access_for_each_transmitter(self):
        for board in (HELTEC, STICK_LITE, XIAO):
            with (
                self.subTest(board=board.name),
                tempfile.TemporaryDirectory() as directory,
            ):
                output = Path(directory) / "lifecycle"
                result = self.compile(output, ["CAJUI_RUNTIME_ROLE=1"], board)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(output)], check=True)

    def test_unsupported_profiles_fail_at_build_time(self):
        transmitter = ["CAJUI_RUNTIME_ROLE=1"]
        cases = (
            (transmitter, None, "board_profile.h"),
            (["CAJUI_RUNTIME_ROLE=2"], XIAO, "supports transmitters only"),
            (transmitter, INCOMPLETE, "OledSda"),
            (transmitter, NO_SENSOR, "needs at least one sensor pin"),
        )
        for definitions, board, message in cases:
            with self.subTest(definitions=definitions, board=board and board.name):
                result = self.compile(None, definitions, board, lifecycle=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                # A missing profile stops at profile.h, before power.cpp uses any board name.
                if board is None:
                    self.assertNotIn("Vext", result.stderr)
