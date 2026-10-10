# SPDX-License-Identifier: Apache-2.0
"""Release smoke tests against compiled ESP32 images; never access a physical device."""

import json
from pathlib import Path
import tempfile
import unittest

from tools import firmware_layout as geometry
from tools import package_firmware as package
from tools.release_assets import assemble


class BuiltAssetsTests(unittest.TestCase):
    def test_installer_and_signed_images_use_the_compiled_layout(self):
        layout = geometry.load()
        boot = (
            Path.home()
            / ".platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
        )
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            private, public = root / "key.pem", root / "key.der"
            package.generate_key(private, public)
            for role, name in [("tx", "transmitter"), ("rx", "receiver")]:
                with self.subTest(role=role):
                    build = Path(".pio/build") / f"runtime_{role}"
                    assemble(build, boot, root / "site", name, "1.2.3")
                    full = root / "site" / f"cajui-{name}-1.2.3-full.bin"
                    self.assertLessEqual(full.stat().st_size, layout["cajui"].offset)
                    self.assertGreaterEqual(
                        full.stat().st_size,
                        layout["ota_0"].offset + (build / "firmware.bin").stat().st_size,
                    )
                    self.assertEqual(
                        b"\xff" * layout["cajui"].size,
                        (root / "site/blank-storage.bin").read_bytes(),
                    )
                    first = json.loads((root / "site" / f"new-{name}.json").read_text())
                    self.assertEqual(
                        layout["cajui"].offset, first["builds"][0]["parts"][1]["offset"]
                    )
                    image = (root / "images" / f"{name}.bin").read_bytes()
                    signed = package.package(image, role, 10203, private)
                    self.assertEqual(10203, package.verify(signed, public.read_bytes(), role))
                    with self.assertRaises(package.PackageError):
                        package.verify(
                            signed[:-1] + bytes([signed[-1] ^ 1]), public.read_bytes(), role
                        )
