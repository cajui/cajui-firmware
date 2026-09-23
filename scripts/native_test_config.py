"""Locate the host OpenSSL installation without embedding machine-specific paths."""

import os
import subprocess
import sys

Import("env")  # noqa: F821 -- SCons/PlatformIO
root = os.environ.get("OPENSSL_ROOT_DIR")
if not root and sys.platform == "darwin":
    root = subprocess.check_output(["brew", "--prefix", "openssl@3"], text=True).strip()
if root:
    env.Append(CPPPATH=[os.path.join(root, "include")], LIBPATH=[os.path.join(root, "lib")])
env.Append(LIBS=["crypto"])
if os.environ.get("CAJUI_COVERAGE") == "1":
    env.Append(
        CCFLAGS=["-fprofile-instr-generate", "-fcoverage-mapping"],
        LINKFLAGS=["-fprofile-instr-generate"],
    )
