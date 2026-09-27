#!/usr/bin/env python3
"""Run the repository's clang-tidy checks over src/ with the ESP32 build's flags.

The host lint (check_protocol.py --lint) covers lib/ and test/. The board code in src/ only
compiles against the Arduino-ESP32 and ESP-IDF headers, so this script takes each image's
compile database from PlatformIO, swaps the Xtensa GCC for clang (dropping GCC-only flags and
adding the toolchain's own include directories), and runs the pinned clang-tidy on src/.
Diagnostics in framework headers are not reported; only src/ must be clean.
"""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENVS = ("runtime_tx", "runtime_rx")
# The same pin as check_protocol.py: uvx locally, the hash-pinned install in CI.
CLANG_TIDY = (
    ["uvx", "--from", "clang-tidy==19.1.0", "clang-tidy"]
    if shutil.which("uvx") and not os.environ.get("CI")
    else ["clang-tidy"]
)
GCC_ONLY = {
    "-mlongcalls",
    "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion",
    "-freorder-blocks",
    "-fno-jump-tables",
    "-MMD",
    "-ggdb",
    "-Werror",
}


def pio():
    local = Path.home() / ".platformio/penv/bin/pio"
    return [str(local)] if local.exists() else ["pio"]


def toolchain_includes(compiler):
    probe = subprocess.run(
        [compiler, "-E", "-x", "c++", "-", "-v"],
        input="",
        capture_output=True,
        text=True,
        check=True,
    ).stderr.splitlines()
    start = probe.index("#include <...> search starts here:") + 1
    end = probe.index("End of search list.")
    return [line.strip() for line in probe[start:end]]


def clang_arguments(command):
    arguments = shlex.split(command)
    compiler, rest = arguments[0], arguments[1:]
    kept, skip = [], False
    for argument in rest:
        if skip:
            skip = False
            continue
        if argument == "-o":
            skip = True
            continue
        if argument in GCC_ONLY:
            continue
        kept.append(argument)
    includes = []
    for directory in toolchain_includes(compiler):
        includes += ["-isystem", directory]
    # A 32-bit little-endian target keeps type sizes close to the ESP32's; the headers
    # select Xtensa code paths through the macro, which clang only parses here.
    return (
        [
            "clang++",
            "--target=i686-unknown-linux-gnu",
            "-nostdinc",
            "-nostdinc++",
            "-D__XTENSA__=1",
            "-Wno-everything",
        ]
        + includes
        + kept
    )


def main():
    entries, seen = [], set()
    for env in ENVS:
        subprocess.run(
            pio() + ["run", "-e", env, "-t", "compiledb"],
            cwd=ROOT,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        for entry in json.loads((ROOT / "compile_commands.json").read_text()):
            file = (
                os.path.relpath(entry["file"], ROOT)
                if os.path.isabs(entry["file"])
                else entry["file"]
            )
            if not file.startswith("src/") or (env, file) in seen:
                continue
            seen.add((env, file))
            entries.append(
                {
                    "directory": str(ROOT),
                    "file": file,
                    "arguments": clang_arguments(entry["command"]),
                    "env": env,
                }
            )
    failed = False
    for env in ENVS:
        chosen = [e for e in entries if e["env"] == env]
        with tempfile.TemporaryDirectory() as folder:
            Path(folder, "compile_commands.json").write_text(
                json.dumps([{k: e[k] for k in ("directory", "file", "arguments")} for e in chosen])
            )
            files = sorted({e["file"] for e in chosen})
            result = subprocess.run(CLANG_TIDY + ["--quiet", "-p", folder] + files, cwd=ROOT)
            failed = failed or result.returncode != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
