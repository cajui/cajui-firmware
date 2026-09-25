#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package signed firmware updates and web-installer manifests (docs/updates.md).

Signing uses the openssl command line; the private key never leaves the machine or CI job
that holds it.
"""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

MAGIC = b"CJFW"
FORMAT = 1
ROLES = {"tx": 1, "rx": 2}
CONTEXT = b"cajui-firmware-v1"
SIGNATURE_CAPACITY = 72  # Largest DER encoding of a P-256 ECDSA signature.
HEADER_SIZE = 16 + 1 + SIGNATURE_CAPACITY
MAX_IMAGE = 0x300000  # One application partition (partitions.csv).


class PackageError(Exception):
    pass


def version_code(text):
    """v1.2.3 -> 10203; each part 0..99. Local builds use 0, which accepts any update."""
    match = re.fullmatch(r"v?(\d{1,2})\.(\d{1,2})\.(\d{1,2})", text)
    if not match:
        raise PackageError("Version must look like 1.2.3 with parts up to 99")
    major, minor, patch = (int(part) for part in match.groups())
    return major * 10000 + minor * 100 + patch


def signed_header(role, version, size):
    if role not in ROLES:
        raise PackageError("Role must be tx or rx")
    if not 0 < size <= MAX_IMAGE:
        raise PackageError("Image must fit one application partition")
    return MAGIC + struct.pack(">BBHII", FORMAT, ROLES[role], 0, version, size)


def openssl(*arguments, data=None):
    try:
        result = subprocess.run(["openssl", *arguments], input=data, capture_output=True)
    except FileNotFoundError:
        raise PackageError("openssl is required") from None
    if result.returncode:
        raise PackageError("openssl failed: " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def sign(message, key):
    signature = openssl("dgst", "-sha256", "-sign", str(key), "-binary", data=message)
    if not 8 <= len(signature) <= SIGNATURE_CAPACITY:
        raise PackageError("Unexpected signature size")
    return signature


def package(image, role, version, key):
    header = signed_header(role, version, len(image))
    signature = sign(CONTEXT + header + image, key)
    return header + bytes([len(signature)]) + signature.ljust(SIGNATURE_CAPACITY, b"\0") + image


def public_der(key):
    return openssl("pkey", "-in", str(key), "-pubout", "-outform", "DER")


def generate_key(private, public):
    """Create a P-256 signing key; refuses to overwrite an existing private key."""
    private = Path(private)
    fd = os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as output:
        output.write(openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256"))
    Path(public).write_bytes(public_der(private))


def c_array(name, data):
    rows = [
        "    " + ", ".join(f"0x{byte:02x}" for byte in data[i : i + 12]) + ","
        for i in range(0, len(data), 12)
    ]
    return f"constexpr uint8_t {name}[] = {{\n" + "\n".join(rows) + "\n};\n"


def key_header(public, name="ReleaseKey"):
    return (
        "// SPDX-License-Identifier: Apache-2.0\n#pragma once\n#include <cstdint>\n\n"
        "// Public key that signs release firmware (tools/package_firmware.py key-header).\n"
        f"namespace cajui {{\n{c_array(name, public)}}} // namespace cajui\n"
    )


def manifest(name, version, image):
    """ESP Web Tools manifest: one merged image at offset 0, never erasing the device."""
    return {
        "name": name,
        "version": version,
        "new_install_prompt_erase": False,
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": image, "offset": 0}]}],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    pack = sub.add_parser("package", help="Sign an application image")
    pack.add_argument("--image", type=Path, required=True)
    pack.add_argument("--role", choices=sorted(ROLES), required=True)
    pack.add_argument("--version", required=True, help="1.2.3")
    pack.add_argument("--key", type=Path, required=True, help="PEM private key")
    pack.add_argument("--output", type=Path, required=True)
    keys = sub.add_parser("generate-key", help="Create a P-256 signing key pair")
    keys.add_argument("--private", type=Path, required=True)
    keys.add_argument("--public", type=Path, required=True, help="DER public key")
    header = sub.add_parser("key-header", help="Write the public key as a C++ header")
    header.add_argument("--public", type=Path, required=True)
    header.add_argument("--output", type=Path, required=True)
    info = sub.add_parser("manifest", help="Write an ESP Web Tools manifest")
    info.add_argument("--name", required=True)
    info.add_argument("--version", required=True)
    info.add_argument("--image", required=True, help="Path of the merged image on the site")
    info.add_argument("--output", type=Path, required=True)
    code = sub.add_parser("version-code", help="Print the numeric version of 1.2.3")
    code.add_argument("version")
    args = parser.parse_args()
    try:
        if args.action == "package":
            data = package(args.image.read_bytes(), args.role, version_code(args.version), args.key)
            with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as output:
                output.write(data)
            os.chmod(output.name, 0o644)  # A release asset, not a secret.
            os.replace(output.name, args.output)
        elif args.action == "generate-key":
            generate_key(args.private, args.public)
        elif args.action == "key-header":
            args.output.write_text(key_header(args.public.read_bytes()))
        elif args.action == "manifest":
            text = json.dumps(manifest(args.name, args.version, args.image), indent=2)
            args.output.write_text(text + "\n")
        else:
            print(version_code(args.version))
    except (PackageError, OSError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
