# SPDX-License-Identifier: Apache-2.0
"""Assemble installer assets using validated CSV and compiled partition tables."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from tools import firmware_layout as geometry
from tools.package_firmware import manifest, version_code


def assemble(build, boot_app0, output, name, version, partitions=geometry.DEFAULT_PARTITIONS):
    version_code(version)
    if name not in ("transmitter", "receiver"):
        raise geometry.LayoutError("Unknown release role")
    layout = geometry.load(partitions)
    geometry.check_binary((build / "partitions.bin").read_bytes(), layout)
    inputs = [
        (0, build / "bootloader.bin", geometry.TABLE_OFFSET),
        (geometry.TABLE_OFFSET, build / "partitions.bin", geometry.TABLE_SIZE),
        (layout["otadata"].offset, boot_app0, layout["otadata"].size),
        (layout["ota_0"].offset, build / "firmware.bin", geometry.image_limit(layout)),
    ]
    for _, path, limit in inputs:
        if not 0 < path.stat().st_size <= limit:
            raise geometry.LayoutError(f"Invalid image size: {path.name}")
    output.mkdir(parents=True, exist_ok=True)
    full = f"cajui-{name}-{version}-full.bin"
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        str(output / full),
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "8MB",
    ]
    for offset, path, _ in inputs:
        command += [hex(offset), str(path)]
    subprocess.run(command, check=True)
    if (output / full).stat().st_size > layout["cajui"].offset:
        raise geometry.LayoutError("Merged image reaches persistent storage")
    (output / "blank-storage.bin").write_bytes(b"\xff" * layout["cajui"].size)
    for mode, erase in (("update", None), ("new", "blank-storage.bin")):
        data = manifest(f"Cajuí {name}", version, full, erase, layout)
        (output / f"{mode}-{name}.json").write_text(json.dumps(data, indent=2) + "\n")
    images = output.parent / "images"
    images.mkdir(exist_ok=True)
    shutil.copyfile(build / "firmware.bin", images / f"{name}.bin")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--boot-app0", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", choices=("transmitter", "receiver"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--partitions", type=Path, default=geometry.DEFAULT_PARTITIONS)
    args = parser.parse_args()
    assemble(args.build, args.boot_app0, args.output, args.name, args.version, args.partitions)


if __name__ == "__main__":
    main()
