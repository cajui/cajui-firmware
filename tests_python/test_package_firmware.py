# SPDX-License-Identifier: Apache-2.0
"""Tests for the firmware packaging tool; they use the openssl command line."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "package_firmware", Path(__file__).resolve().parents[1] / "tools/package_firmware.py"
)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.dir = Path(self.temporary.name)
        self.key, self.public = self.dir / "key.pem", self.dir / "key.der"
        tool.generate_key(self.key, self.public)

    def tearDown(self):
        self.temporary.cleanup()

    def test_version_codes(self):
        self.assertEqual(10203, tool.version_code("1.2.3"))
        self.assertEqual(990000, tool.version_code("v99.0.0"))
        for bad in ("1.2", "100.0.0", "1.2.3-rc1", "a.b.c", "0.0.0"):
            with self.subTest(bad=bad), self.assertRaises(tool.PackageError):
                tool.version_code(bad)

    def test_package_layout_and_signature_verify_with_openssl(self):
        image = bytes(range(256)) * 4
        data = tool.package(image, "rx", 10203, self.key)
        self.assertEqual(tool.HEADER_SIZE + len(image), len(data))
        magic, fmt, role, reserved, version, size = struct.unpack(">4sBBHII", data[:16])
        self.assertEqual(
            (b"CJFW", 1, 2, 0, 10203, len(image)), (magic, fmt, role, reserved, version, size)
        )
        length = data[16]
        signature = data[17 : 17 + length]
        self.assertEqual(
            bytes(tool.SIGNATURE_CAPACITY - length), data[17 + length : tool.HEADER_SIZE]
        )
        pem = self.dir / "public.pem"
        pem.write_bytes(tool.openssl("pkey", "-pubin", "-inform", "DER", "-in", str(self.public)))
        (self.dir / "sig").write_bytes(signature)
        message = tool.CONTEXT + data[:16] + image
        result = subprocess.run(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-verify",
                str(pem),
                "-signature",
                str(self.dir / "sig"),
            ],
            input=message,
            capture_output=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)

    def test_invalid_inputs(self):
        with self.assertRaises(tool.PackageError):
            tool.signed_header("xx", 1, 10)
        for size in (0, tool.MAX_IMAGE + 1):
            with self.subTest(size=size), self.assertRaises(tool.PackageError):
                tool.signed_header("tx", 1, size)
        with self.assertRaises(tool.PackageError):
            tool.sign(b"x", self.dir / "missing.pem")
        with self.assertRaises(FileExistsError):
            tool.generate_key(self.key, self.public)  # Never overwrites a private key.
        self.assertEqual(0o600, self.key.stat().st_mode & 0o777)
        with patch.object(tool.subprocess, "run", side_effect=FileNotFoundError):
            with self.assertRaisesRegex(tool.PackageError, "openssl"):
                tool.openssl("version")
        with patch.object(tool, "openssl", return_value=b"x"):
            with self.assertRaisesRegex(tool.PackageError, "signature size"):
                tool.sign(b"x", self.key)

    def test_key_header_and_manifest(self):
        text = tool.key_header(bytes(range(20)), "TestKey")
        self.assertIn("constexpr uint8_t TestKey[] = {", text)
        self.assertIn("0x13,", text)
        self.assertEqual(bytes(range(20)), tool.key_from_header(text))
        manifest = tool.manifest("Cajuí receiver", "1.2.3", "receiver.bin")
        self.assertFalse(manifest["new_install_prompt_erase"])
        self.assertEqual([{"path": "receiver.bin", "offset": 0}], manifest["builds"][0]["parts"])
        first = tool.manifest("Cajuí receiver", "1.2.3", "receiver.bin", "blank.bin")
        self.assertFalse(first["new_install_prompt_erase"])
        self.assertEqual({"path": "blank.bin", "offset": 0x310000}, first["builds"][0]["parts"][1])

    def test_verify_matches_the_device_rules(self):
        image = bytes(range(200))
        data = tool.package(image, "rx", 10203, self.key)
        public = self.public.read_bytes()
        self.assertEqual(10203, tool.verify(data, public, "rx"))
        self.assertEqual(10203, tool.verify(data, public))
        other_key, other_public = self.dir / "other.pem", self.dir / "other.der"
        tool.generate_key(other_key, other_public)
        cases = {
            "tampered image": data[:-1] + bytes([data[-1] ^ 1]),
            "tampered version": data[:11] + bytes([data[11] ^ 1]) + data[12:],
            "padding": data[: tool.HEADER_SIZE - 1] + b"\x01" + data[tool.HEADER_SIZE :],
            "truncated": data[:-1],
            "not an update": b"XXXX" + data[4:],
            "other key": tool.package(image, "rx", 10203, other_key),
        }
        for name, bad in cases.items():
            with self.subTest(name=name), self.assertRaises(tool.PackageError):
                tool.verify(bad, public, "rx")
        with self.assertRaisesRegex(tool.PackageError, "role"):
            tool.verify(data, public, "tx")

    def test_failed_key_generation_leaves_no_file(self):
        private = self.dir / "failed.pem"
        with patch.object(tool, "openssl", side_effect=tool.PackageError("boom")):
            with self.assertRaises(tool.PackageError):
                tool.generate_key(private, self.dir / "failed.der")
        self.assertFalse(private.exists())

    def run_cli(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            patch.object(sys, "argv", ["package_firmware.py", *arguments]),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = tool.main()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_command_line(self):
        image = self.dir / "firmware.bin"
        image.write_bytes(b"\xe9" + bytes(100))
        output = self.dir / "update.cjfw"
        code, _, _ = self.run_cli(
            "package",
            "--image",
            str(image),
            "--role",
            "tx",
            "--version",
            "0.1.0",
            "--key",
            str(self.key),
            "--output",
            str(output),
        )
        self.assertEqual(0, code)
        self.assertEqual(tool.HEADER_SIZE + 101, output.stat().st_size)
        header = self.dir / "key.h"
        self.assertEqual(
            0, self.run_cli("key-header", "--public", str(self.public), "--output", str(header))[0]
        )
        self.assertIn("ReleaseKey", header.read_text())
        code, text, _ = self.run_cli(
            "verify", "--key-header", str(header), "--role", "tx", str(output)
        )
        self.assertEqual(0, code)
        self.assertIn("signed, version code 100", text)
        code, _, error = self.run_cli(
            "verify", "--key-header", str(header), "--role", "rx", str(output)
        )
        self.assertEqual(1, code)
        self.assertIn("role", error)
        manifest = self.dir / "manifest.json"
        self.assertEqual(
            0,
            self.run_cli(
                "manifest",
                "--name",
                "n",
                "--version",
                "1.0.0",
                "--image",
                "a.bin",
                "--erase-storage",
                "b.bin",
                "--output",
                str(manifest),
            )[0],
        )
        self.assertEqual("n", json.loads(manifest.read_text())["name"])
        self.assertEqual((0, "10000\n", ""), self.run_cli("version-code", "1.0.0"))
        code, _, error = self.run_cli("version-code", "bad")
        self.assertEqual(1, code)
        self.assertIn("Packaging failed", error)
        other = self.dir / "other"
        other.mkdir()
        self.assertEqual(
            0,
            self.run_cli(
                "generate-key", "--private", str(other / "k.pem"), "--public", str(other / "k.der")
            )[0],
        )


if __name__ == "__main__":
    unittest.main()
