#!/usr/bin/env python3
"""Run native Unity tests and optional LLVM coverage checks without accessing boards."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coverage", action="store_true", help="Requires Clang and LLVM")
    args = parser.parse_args()
    pio = ["pio"] if shutil.which("pio") else ["uvx", "--from", "platformio==6.1.18", "pio"]
    environment = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix="cajui-protocol-") as temporary:
        folder = Path(temporary)
        if args.coverage:
            environment["CAJUI_COVERAGE"] = "1"
            environment["LLVM_PROFILE_FILE"] = str(folder / "profile.profraw")
        else:
            environment.pop("CAJUI_COVERAGE", None)
        run(pio + ["test", "-e", "native"], env=environment)
        run([sys.executable, "-m", "unittest", "discover", "-s", "tests_python", "-v"])
        if args.coverage:
            prefix = ["xcrun"] if shutil.which("xcrun") else []
            profile = str(folder / "merged.profdata")
            run(prefix + ["llvm-profdata", "merge", "-sparse", str(folder / "profile.profraw"),
                          "-o", profile])
            inputs = [".pio/build/native/program", f"-instr-profile={profile}",
                      "lib/CajuiProtocol/src/cajui_protocol.cpp", "lib/CajuiProtocol/src/crypto.cpp",
                      "lib/CajuiStorage/src/cajui_storage.cpp",
                      "lib/CajuiProvisioning/src/cajui_provisioning.cpp"]
            run(prefix + ["llvm-cov", "report"] + inputs)
            report = subprocess.check_output(prefix + ["llvm-cov", "export"] + inputs,
                                             cwd=ROOT, text=True)
            totals = json.loads(report)["data"][0]["totals"]
            if totals["lines"]["percent"] < 95 or totals["branches"]["percent"] < 85:
                raise SystemExit("Coverage below minimum: 95% lines and 85% branches (host).")


if __name__ == "__main__":
    main()
