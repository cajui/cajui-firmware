#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run native Unity tests, optional LLVM coverage and lint checks without accessing boards."""

import argparse
import json
import importlib.metadata
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
# Pinned so local runs and CI format and lint identically.
TOOLS = {
    "clang-format": "clang-format==23.1.2",
    "clang-tidy": "clang-tidy==19.1.0",
    "ruff": "ruff==0.16.10",
    "shellcheck": "shellcheck-py==0.10.0.1",
    "actionlint": "actionlint-py==1.7.7.24",
}
PYTHON = ["tools", "tests_python", "tests_release", "scripts"]


def run(command, **kwargs):
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def pio():
    return ["pio"] if shutil.which("pio") else ["uvx", "--from", "platformio==6.2.0", "pio"]


def tool(name):
    # uvx guarantees the pinned version locally; CI installs the same pins with hashes and
    # must use those, even if a runner image ships uv.
    local = shutil.which("uvx") and not os.environ.get("CI")
    if local:
        dependencies = ["--with", TOOLS["shellcheck"]] if name == "actionlint" else []
        return ["uvx", *dependencies, "--from", TOOLS[name], name]
    if not shutil.which(name):
        raise SystemExit(
            f"Missing {name}. Install uv or install {TOOLS[name]} in a virtual environment."
        )
    return [name]


def tracked(*patterns):
    output = subprocess.check_output(["git", "ls-files", *patterns], cwd=ROOT, text=True)
    return output.split()


PYTHON_FLOORS = {
    path: (95, 95)
    for path in (
        "tools/provision.py",
        "tools/package_firmware.py",
        "tools/firmware_layout.py",
        "tools/release_assets.py",
        "tools/release_check.py",
        "tools/release_publish.py",
    )
}
COVERAGE_VERSION = "7.16.2"


def python_coverage_command():
    try:
        installed = importlib.metadata.version("coverage")
    except importlib.metadata.PackageNotFoundError:
        installed = None
    if installed != COVERAGE_VERSION:
        raise SystemExit(
            f"Install coverage=={COVERAGE_VERSION} in {sys.executable} before --coverage."
        )
    return [sys.executable, "-m", "coverage"]


def check_python_coverage(report, floors=PYTHON_FLOORS):
    for path, (line_floor, branch_floor) in floors.items():
        try:
            summary = report["files"][path]["summary"]
            counts = (
                ("lines", summary["covered_lines"], summary["num_statements"], line_floor),
                ("branches", summary["covered_branches"], summary["num_branches"], branch_floor),
            )
        except (KeyError, TypeError):
            raise SystemExit(f"Missing Python line/branch coverage data for {path}.") from None
        for label, covered, total, floor in counts:
            if total <= 0 or covered * 100 < total * floor:
                raise SystemExit(f"{path}: {label} coverage below {floor}%: {covered}/{total}.")
            print(f"{path}: {label} {covered}/{total} ({100 * covered / total:.2f}%)")


# Host implementation files under the coverage gate; README and docs/testing.md list them.
GATED_FILES = (
    "lib/CajuiSensors/src/cajui_sht4x.cpp",
    "lib/CajuiProtocol/src/codec.cpp",
    "lib/CajuiProtocol/src/delivery.cpp",
    "lib/CajuiRuntime/src/cajui_runtime.cpp",
    "lib/CajuiApplication/src/cajui_application.cpp",
    "lib/CajuiProtocol/src/crypto.cpp",
    "lib/CajuiStorage/src/cajui_storage.cpp",
    "lib/CajuiStorage/src/snapshot.cpp",
    "lib/CajuiStorage/src/cajui_crc32.cpp",
    "lib/CajuiStorage/src/records.cpp",
    "lib/CajuiProvisioning/src/cajui_provisioning.cpp",
    "lib/CajuiUplink/src/cajui_uplink.cpp",
    "lib/CajuiUplink/src/cajui_manage.cpp",
    "lib/CajuiUplink/src/cajui_command.cpp",
    "lib/CajuiUplink/src/cajui_discovery.cpp",
    "lib/CajuiSetup/src/cajui_setup.cpp",
    "lib/CajuiSetup/src/cajui_setup_save.cpp",
    "lib/CajuiPairing/src/cajui_pairing.cpp",
    "lib/CajuiDevice/src/cajui_device.cpp",
    "lib/CajuiDevice/src/cajui_service.cpp",
    "lib/CajuiFirmware/src/cajui_firmware.cpp",
)
# Every gated host file must reach both minima on its own; an aggregate would let a
# well-covered file hide a poorly covered one.
LINE_MINIMUM, BRANCH_MINIMUM = 95, 85
# Documented exceptions. crypto.cpp: its remaining host branches are OpenSSL allocation and
# EVP failure returns, which no test can trigger without fault injection into the library;
# known-answer vectors (NIST GCM, RFC 7748, RFC 5869) cover the success paths.
BRANCH_FLOORS = {"lib/CajuiProtocol/src/crypto.cpp": 60}


BOARD_ONLY_FILES = {"lib/CajuiStorage/src/cajui_nvs.cpp": "ESP-IDF NVS adapter"}
BRANCHLESS_FILES = frozenset()


def check_coverage_inventory(
    sources, expected=GATED_FILES, excluded=BOARD_ONLY_FILES, branchless=BRANCHLESS_FILES
):
    sources, expected, excluded = set(sources), set(expected), set(excluded)
    failures = [
        f"{path}: missing coverage policy" for path in sorted(sources - expected - excluded)
    ]
    failures += [
        f"{path}: stale coverage policy" for path in sorted((expected | excluded) - sources)
    ]
    failures += [f"{path}: both gated and excluded" for path in sorted(expected & excluded)]
    failures += [
        f"{path}: branchless exception is not gated" for path in sorted(set(branchless) - expected)
    ]
    return failures


def check_native_coverage(export, expected=GATED_FILES, branchless=BRANCHLESS_FILES):
    """Return one message per gated file below its minimum or missing from the report."""
    failures = []
    try:
        entries = export["data"][0]["files"]
    except (KeyError, IndexError, TypeError):
        return ["coverage report has no file data"]
    seen = set()
    for entry in entries:
        name = entry["filename"]
        # Match on the repository-relative suffix, not the first "lib/" of the absolute
        # path (a checkout can live under /var/lib).
        path = next((gated for gated in expected if name.endswith("/" + gated)), name)
        summary = entry["summary"]
        lines, branches = summary["lines"], summary["branches"]
        seen.add(path)
        floor = BRANCH_FLOORS.get(path, BRANCH_MINIMUM)
        if not lines["count"]:
            failures.append(f"{path}: no counted lines")
        elif lines["percent"] < LINE_MINIMUM:
            failures.append(f"{path}: lines {lines['percent']:.2f}% < {LINE_MINIMUM}%")
        elif not branches["count"] and path not in branchless:
            failures.append(
                f"{path}: no counted branches; verify instrumentation or declare branchless"
            )
        if branches["count"] and branches["percent"] < floor:
            failures.append(f"{path}: branches {branches['percent']:.2f}% < {floor}%")
    failures += [f"{path}: no coverage data" for path in expected if path not in seen]
    return failures


def lint():
    sources = tracked("lib/*.cpp", "lib/*.h", "src/*.cpp", "test/*.cpp", "test/*.h")
    run(tool("clang-format") + ["--dry-run", "--Werror"] + sources)
    run(tool("ruff") + ["format", "--check"] + PYTHON)
    run(tool("ruff") + ["check"] + PYTHON)
    run(tool("shellcheck") + ["--severity=style", *tracked("*.sh")])
    run(tool("actionlint") + ["-color"])
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
    parser.add_argument(
        "--python-only", action="store_true", help="Run Python tests without building C++"
    )
    parser.add_argument("--lint", action="store_true", help="Run formatters and linters only")
    args = parser.parse_args()
    if sys.version_info < (3, 10):
        parser.error("Python 3.10 or newer is required")
    failures = check_coverage_inventory(
        path.relative_to(ROOT).as_posix() for path in ROOT.glob("lib/*/src/*.cpp")
    )
    if failures:
        raise SystemExit("\n".join(failures))
    coverage = python_coverage_command() if args.coverage else None
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
        if not args.python_only:
            run(pio() + ["test", "-e", "native"], env=environment)
        unittest = ["-m", "unittest", "discover", "-s", "tests_python", "-v"]
        if args.coverage:
            data = f"--data-file={folder / 'python.coverage'}"
            run(
                coverage
                + ["run", "--branch", "--include=" + ",".join(PYTHON_FLOORS), data]
                + unittest
            )
            run(coverage + ["report", "-m", data])
            python_report = folder / "python-coverage.json"
            run(coverage + ["json", data, "-o", str(python_report)])
            check_python_coverage(json.loads(python_report.read_text()))
        else:
            run([sys.executable] + unittest)
        if args.coverage and not args.python_only:
            prefix = ["xcrun"] if shutil.which("xcrun") else []
            profile = str(folder / "merged.profdata")
            run(
                prefix
                + [
                    os.environ.get("LLVM_PROFDATA", "llvm-profdata"),
                    "merge",
                    "-sparse",
                    str(folder / "profile.profraw"),
                    "-o",
                    profile,
                ]
            )
            inputs = [".pio/build/native/program", f"-instr-profile={profile}", *GATED_FILES]
            run(prefix + [os.environ.get("LLVM_COV", "llvm-cov"), "report"] + inputs)
            report = subprocess.check_output(
                prefix + [os.environ.get("LLVM_COV", "llvm-cov"), "export"] + inputs,
                cwd=ROOT,
                text=True,
            )
            failures = check_native_coverage(json.loads(report))
            if failures:
                raise SystemExit("Coverage below the per-file minimum:\n" + "\n".join(failures))


if __name__ == "__main__":
    main()
