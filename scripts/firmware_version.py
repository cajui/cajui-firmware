# SPDX-License-Identifier: Apache-2.0
"""Embed the numeric firmware version: CAJUI_FIRMWARE_VERSION from the environment, else 0.

Release builds set it from the tag (tools/package_firmware.py version-code); 0 marks a
local build, which accepts any signed update.
"""

import os

Import("env")  # noqa: F821 -- SCons/PlatformIO
version = os.environ.get("CAJUI_FIRMWARE_VERSION", "0")
if not version.isdigit():
    raise SystemExit("CAJUI_FIRMWARE_VERSION must be a number")
env.Append(CPPDEFINES=[("CAJUI_FIRMWARE_VERSION", version)])  # noqa: F821
