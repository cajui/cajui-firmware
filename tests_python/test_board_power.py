# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests_python/fixtures/board"


class BoardPowerTests(unittest.TestCase):
    def compile(self, output, definitions, *, lifecycle=True):
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
            *[f"-D{definition}" for definition in definitions],
        ]
        if lifecycle:
            command += [*[str(path) for path in sources], "-o", str(output)]
            return subprocess.run(command, capture_output=True, text=True, check=False)
        command += ["-fsyntax-only", "-x", "c++", "-"]
        return subprocess.run(
            command,
            input='#include "board/profile.h"\n',
            capture_output=True,
            text=True,
            check=False,
        )

    def test_sleep_wake_and_battery_access_for_each_transmitter(self):
        for board, sensor in (
            (None, "CAJUI_SENSOR_DHT22"),
            ("CAJUI_BOARD_STICK_LITE", "CAJUI_SENSOR_SHT4X"),
            ("CAJUI_BOARD_XIAO_S3", "CAJUI_SENSOR_SHT4X"),
            ("CAJUI_BOARD_XIAO_S3", "CAJUI_SENSOR_DHT22"),
        ):
            with (
                self.subTest(board=board, sensor=sensor),
                tempfile.TemporaryDirectory() as directory,
            ):
                output = Path(directory) / "lifecycle"
                definitions = ["CAJUI_RUNTIME_ROLE=1", sensor]
                if board:
                    definitions.append(board)
                result = self.compile(output, definitions)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(output)], check=True)

    def test_unsupported_profiles_fail_at_build_time(self):
        cases = (
            (["CAJUI_RUNTIME_ROLE=1"], "Select a climate sensor"),
            (
                ["CAJUI_RUNTIME_ROLE=2", "CAJUI_BOARD_XIAO_S3"],
                "supports transmitters only",
            ),
            (
                ["CAJUI_RUNTIME_ROLE=1", "CAJUI_SENSOR_SHT4X"],
                "SHT4x requires I2C pins",
            ),
            (
                ["CAJUI_RUNTIME_ROLE=1", "CAJUI_BOARD_STICK_LITE", "CAJUI_SENSOR_DHT22"],
                "DHT22 requires a data pin",
            ),
            (
                [
                    "CAJUI_RUNTIME_ROLE=1",
                    "CAJUI_BOARD_XIAO_S3",
                    "CAJUI_BOARD_STICK_LITE",
                    "CAJUI_SENSOR_SHT4X",
                ],
                "Select exactly one board",
            ),
        )
        for definitions, message in cases:
            with self.subTest(definitions=definitions):
                result = self.compile(None, definitions, lifecycle=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
