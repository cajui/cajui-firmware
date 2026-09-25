#!/usr/bin/env python3
"""Run native Unity tests, optional LLVM coverage and lint checks without accessing boards."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
# Pinned so local runs and CI format and lint identically.
TOOLS = {
    "coverage": "coverage==7.6.1",
    "clang-format": "clang-format==19.1.7",
    "clang-tidy": "clang-tidy==19.1.0",
    "ruff": "ruff==0.6.9",
}
PYTHON = ["tools", "tests_python", "scripts"]


def run(command, **kwargs):
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def pio():
    return ["pio"] if shutil.which("pio") else ["uvx", "--from", "platformio==6.1.18", "pio"]


def tool(name):
    # uvx guarantees the pinned version; CI installs the same pins with pip instead.
    return ["uvx", "--from", TOOLS[name], name] if shutil.which("uvx") else [name]


def tracked(*patterns):
    output = subprocess.check_output(["git", "ls-files", *patterns], cwd=ROOT, text=True)
    return output.split()


def check_python_coverage(report):
    """Require independent, unrounded line and branch minima for the USB client."""
    try:
        summary = report["files"]["tools/provision.py"]["summary"]
        counts = (
            ("lines", summary["covered_lines"], summary["num_statements"]),
            ("branches", summary["covered_branches"], summary["num_branches"]),
        )
    except KeyError:
        raise SystemExit("Missing Python client line/branch coverage data.") from None
    for label, covered, total in counts:
        if total <= 0 or covered * 100 < total * 95:
            raise SystemExit(f"Python client {label} coverage below 95%: {covered}/{total}.")
        print(f"Python client {label}: {covered}/{total} ({100 * covered / total:.2f}%)")


def lint():
    sources = tracked("lib/*.cpp", "lib/*.h", "src/*.cpp", "test/*.cpp", "test/*.h")
    run(tool("clang-format") + ["--dry-run", "--Werror"] + sources)
    run(tool("ruff") + ["format", "--check"] + PYTHON)
    run(tool("ruff") + ["check"] + PYTHON)
    # Host-compilable library code only: src/ needs Arduino and test/ is fixture-heavy.
    includes = sorted({str(Path(header).parent) for header in tracked("lib/*/src/*.h")})
    flags = ["-std=c++11"] + [f"-I{path}" for path in includes]
    root = os.environ.get("OPENSSL_ROOT_DIR")
    if not root and sys.platform == "darwin":
        root = subprocess.check_output(["brew", "--prefix", "openssl@3"], text=True).strip()
    if root:
        flags.append(f"-I{Path(root) / 'include'}")
    if shutil.which("xcrun"):
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        flags += ["-isysroot", sdk]
    run(tool("clang-tidy") + ["--quiet"] + tracked("lib/*.cpp") + ["--"] + flags)
    # Tests use test/.clang-tidy and need the pinned Unity headers from PlatformIO.
    run(pio() + ["pkg", "install", "-e", "native", "--silent"])
    unity = next(ROOT.glob(".pio/libdeps/native/Unity/src"))
    run(tool("clang-tidy") + ["--quiet"] + tracked("test/*.cpp") + ["--"] + flags + [f"-I{unity}"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coverage", action="store_true", help="Requires Clang and LLVM")
    parser.add_argument("--lint", action="store_true", help="Run formatters and linters only")
    args = parser.parse_args()
    if args.lint:
        lint()
        return
    environment = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix="cajui-protocol-") as temporary:
        folder = Path(temporary)
        if args.coverage:
            environment["CAJUI_COVERAGE"] = "1"
            environment["LLVM_PROFILE_FILE"] = str(folder / "profile.profraw")
        else:
            environment.pop("CAJUI_COVERAGE", None)
        run(pio() + ["test", "-e", "native"], env=environment)
        unittest = ["-m", "unittest", "discover", "-s", "tests_python", "-v"]
        if args.coverage:
            data = f"--data-file={folder / 'python.coverage'}"
            run(
                tool("coverage")
                + ["run", "--branch", "--include=tools/provision.py", data]
                + unittest
            )
            run(tool("coverage") + ["report", "-m", data])
            python_report = folder / "python-coverage.json"
            run(tool("coverage") + ["json", data, "-o", str(python_report)])
            check_python_coverage(json.loads(python_report.read_text()))
        else:
            run([sys.executable] + unittest)
        if args.coverage:
            prefix = ["xcrun"] if shutil.which("xcrun") else []
            profile = str(folder / "merged.profdata")
            run(
                prefix
                + [
                    "llvm-profdata",
                    "merge",
                    "-sparse",
                    str(folder / "profile.profraw"),
                    "-o",
                    profile,
                ]
            )
            inputs = [
                ".pio/build/native/program",
                f"-instr-profile={profile}",
                "lib/CajuiProtocol/src/codec.cpp",
                "lib/CajuiProtocol/src/delivery.cpp",
                "lib/CajuiRuntime/src/cajui_runtime.cpp",
                "lib/CajuiApplication/src/cajui_application.cpp",
                "lib/CajuiProtocol/src/crypto.cpp",
                "lib/CajuiStorage/src/cajui_storage.cpp",
                "lib/CajuiStorage/src/snapshot.cpp",
                "lib/CajuiStorage/src/cajui_crc32.cpp",
                "lib/CajuiProvisioning/src/cajui_provisioning.cpp",
                "lib/CajuiUplink/src/cajui_uplink.cpp",
                "lib/CajuiSetup/src/cajui_setup.cpp",
                "lib/CajuiPairing/src/cajui_pairing.cpp",
            ]
            run(prefix + ["llvm-cov", "report"] + inputs)
            report = subprocess.check_output(
                prefix + ["llvm-cov", "export"] + inputs, cwd=ROOT, text=True
            )
            totals = json.loads(report)["data"][0]["totals"]
            if totals["lines"]["percent"] < 95 or totals["branches"]["percent"] < 85:
                raise SystemExit("Coverage below minimum: 95% lines and 85% branches (host).")


if __name__ == "__main__":
    main()
