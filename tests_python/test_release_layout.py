# SPDX-License-Identifier: Apache-2.0
import contextlib
import io
import json
from pathlib import Path
import struct
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools import firmware_layout as geometry
from tools import package_firmware as package
from tools import release_assets


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.csv = self.root / "partitions.csv"
        self.original = geometry.DEFAULT_PARTITIONS.read_text()
        self.csv.write_text(self.original)

    def binary(self, layout):
        import hashlib

        entries = b"".join(
            struct.pack(
                "<2sBBII16sI",
                b"\xaa\x50",
                geometry.TYPES[p.kind],
                geometry.SUBTYPES[p.kind, p.subtype],
                p.offset,
                p.size,
                p.name.encode(),
                0,
            )
            for p in layout.values()
        )
        return (entries + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest()).ljust(
            0xC00, b"\xff"
        )

    def test_current_table_and_changed_geometry(self):
        layout = geometry.load(self.csv)
        geometry.check_binary(self.binary(layout), layout)
        changed = (
            self.original.replace("0x300000", "2M")
            .replace("0x310000", "0x210000")
            .replace("0x40000", "128K")
            .replace("0x350000", "0x230000")
        )
        self.csv.write_text(changed)
        smaller = geometry.load(self.csv)
        self.assertEqual(2 * 1024 * 1024, geometry.image_limit(smaller))
        manifest = package.manifest("test", "1.2.3", "app.bin", "erase.bin", smaller)
        self.assertEqual(smaller["cajui"].offset, manifest["builds"][0]["parts"][1]["offset"])
        with self.assertRaises(package.PackageError):
            package.signed_header("rx", 1, geometry.image_limit(smaller) + 1, smaller)
        with self.assertRaises(geometry.LayoutError):
            geometry.check_binary(self.binary(layout), smaller)
        self.assertEqual(42, geometry.number("42"))

    def test_invalid_layouts_fail_closed(self):
        cases = [
            "nvs,data,nvs,0x9000\n",
            self.original.replace("0x5000,", "0x5000,encrypted"),
            self.original + "nvs,data,nvs,0x700000,0x1000,\n",
            self.original.replace("nvs,data,nvs", ",data,nvs", 1),
            self.original.replace("nvs,data,nvs", "x" * 16 + ",data,nvs", 1),
            self.original.replace("nvs,data,nvs", "nvs,bad,nvs", 1),
            self.original.replace("0x9000", "oops"),
            self.original.replace("0x9000", "0x9001"),
            self.original.replace("0x9000", "0x1000"),
            self.original.replace("0x5000", "0"),
            self.original.replace("0x5000", "4097"),
            self.original.replace("0x350000", "0x800000"),
            self.original.replace("0x5000", "0x6000"),
            self.original.replace("cajui,data,nvs", "other,data,nvs"),
            self.original.replace("cajui,data,nvs", "cajui,data,ota"),
            self.original.replace("ota_0,app,ota_0", "ota_0,data,nvs"),
            self.original.replace("ota_0,app,ota_0,0x10000", "ota_0,app,ota_0,0x400000").replace(
                "ota_1,app,ota_1,0x350000", "ota_1,app,ota_1,0x10000"
            ),
            self.original.replace("otadata,data,ota,0xe000", "otadata,data,ota,0x700000"),
        ]
        for data in cases:
            with self.subTest(data=data), self.assertRaises(geometry.LayoutError):
                self.csv.write_text(data)
                geometry.load(self.csv)

    def test_binary_corruption_and_size_are_rejected(self):
        layout = geometry.load(self.csv)
        valid = self.binary(layout)
        for bad in (
            b"",
            valid[:160],
            valid + bytes(8192),
            bytes([0]) + valid[1:],
            valid[:-1] + b"\0",
        ):
            with self.subTest(size=len(bad)), self.assertRaises(geometry.LayoutError):
                geometry.check_binary(bad, layout)

    def test_assembler_uses_layout_offsets_and_blank_size(self):
        self.csv.write_text(
            self.original.replace("0x300000", "2M")
            .replace("0x310000", "0x210000")
            .replace("0x40000", "128K")
            .replace("0x350000", "0x230000")
        )
        layout = geometry.load(self.csv)
        build = self.root / "build"
        build.mkdir()
        for name in ("bootloader.bin", "firmware.bin", "boot_app0.bin"):
            (build / name).write_bytes(b"firmware")
        (build / "partitions.bin").write_bytes(self.binary(layout))
        output = self.root / "out" / "site"

        def merge(command, check):
            self.assertTrue(check)
            self.assertEqual(
                [
                    "0x0",
                    str(build / "bootloader.bin"),
                    "0x8000",
                    str(build / "partitions.bin"),
                    hex(layout["otadata"].offset),
                    str(build / "boot_app0.bin"),
                    hex(layout["ota_0"].offset),
                    str(build / "firmware.bin"),
                ],
                command[-8:],
            )
            Path(command[command.index("-o") + 1]).write_bytes(b"image")

        with (
            patch.object(release_assets.subprocess, "run", side_effect=merge),
            patch.object(
                sys,
                "argv",
                [
                    "release_assets",
                    "--build",
                    str(build),
                    "--boot-app0",
                    str(build / "boot_app0.bin"),
                    "--output",
                    str(output),
                    "--name",
                    "receiver",
                    "--version",
                    "1.2.3",
                    "--partitions",
                    str(self.csv),
                ],
            ),
        ):
            runpy.run_path(str(Path(release_assets.__file__)), run_name="__main__")
        self.assertEqual(
            b"\xff" * layout["cajui"].size, (output / "blank-storage.bin").read_bytes()
        )
        self.assertEqual(
            1, len(json.loads((output / "update-receiver.json").read_text())["builds"][0]["parts"])
        )
        self.assertEqual(
            layout["cajui"].offset,
            json.loads((output / "new-receiver.json").read_text())["builds"][0]["parts"][1][
                "offset"
            ],
        )
        self.assertEqual(b"firmware", (output.parent / "images" / "receiver.bin").read_bytes())
        (build / "firmware.bin").write_bytes(b"")
        with (
            patch.object(release_assets.subprocess, "run") as run,
            self.assertRaises(geometry.LayoutError),
        ):
            release_assets.assemble(
                build, build / "boot_app0.bin", output, "receiver", "1.2.3", self.csv
            )
        run.assert_not_called()
        (build / "firmware.bin").write_bytes(b"firmware")
        with (
            patch.object(release_assets.subprocess, "run"),
            patch.object(Path, "stat") as stat,
            self.assertRaises(geometry.LayoutError),
        ):
            stat.return_value.st_size = geometry.image_limit(layout) + 1
            release_assets.assemble(
                build, build / "boot_app0.bin", output, "receiver", "1.2.3", self.csv
            )
        (output / "cajui-receiver-1.2.3-full.bin").write_bytes(bytes(layout["cajui"].offset + 1))
        with (
            patch.object(release_assets.subprocess, "run"),
            self.assertRaises(geometry.LayoutError),
        ):
            release_assets.assemble(
                build, build / "boot_app0.bin", output, "receiver", "1.2.3", self.csv
            )
        with self.assertRaises(geometry.LayoutError):
            release_assets.assemble(
                build, build / "boot_app0.bin", output, "other", "1.2.3", self.csv
            )

    def test_cli_and_layout_failure_are_reported(self):
        args = [
            "release_assets",
            "--build",
            "build",
            "--boot-app0",
            "boot.bin",
            "--output",
            "out",
            "--name",
            "receiver",
            "--version",
            "1.2.3",
        ]
        with patch.object(sys, "argv", args), patch.object(release_assets, "assemble") as assemble:
            release_assets.main()
        self.assertEqual("receiver", assemble.call_args.args[3])
        self.csv.write_text("invalid")
        with (
            patch.object(
                sys,
                "argv",
                [
                    "package",
                    "manifest",
                    "--name",
                    "x",
                    "--version",
                    "1.2.3",
                    "--image",
                    "x",
                    "--output",
                    str(self.root / "out"),
                    "--partitions",
                    str(self.csv),
                ],
            ),
            contextlib.redirect_stderr(io.StringIO()) as error,
        ):
            self.assertEqual(1, package.main())
        self.assertIn("Packaging failed", error.getvalue())
