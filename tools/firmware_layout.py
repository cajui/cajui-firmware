# SPDX-License-Identifier: Apache-2.0
"""Validated partition geometry for release packaging and USB installer assets."""

import csv
import hashlib
from pathlib import Path
import struct
from typing import NamedTuple

DEFAULT_PARTITIONS = Path(__file__).resolve().parents[1] / "partitions.csv"
FLASH_SIZE = 8 * 1024 * 1024
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0x1000
TYPES = {"app": 0, "data": 1}
SUBTYPES = {("app", "ota_0"): 0x10, ("app", "ota_1"): 0x11, ("data", "nvs"): 2, ("data", "ota"): 0}


class LayoutError(ValueError):
    pass


class Partition(NamedTuple):
    name: str
    kind: str
    subtype: str
    offset: int
    size: int


def number(text):
    text = text.strip().lower()
    multiplier = 1
    if text.endswith(("k", "m")):
        multiplier = 1024 if text[-1] == "k" else 1024 * 1024
        text = text[:-1]
    return int(text, 16 if text.startswith("0x") else 10) * multiplier


def load(path=DEFAULT_PARTITIONS):
    partitions = {}
    for row in csv.reader(Path(path).read_text().splitlines()):
        if not row or row[0].lstrip().startswith("#"):
            continue
        row = [field.strip() for field in row]
        if len(row) not in (5, 6) or (len(row) == 6 and row[5]):
            raise LayoutError("Partitions require explicit offsets and no flags")
        name, kind, subtype, offset, size = row[:5]
        if (
            (kind, subtype) not in SUBTYPES
            or not name
            or len(name.encode()) > 15
            or name in partitions
        ):
            raise LayoutError("Unsupported or duplicate partition")
        try:
            part = Partition(name, kind, subtype, number(offset), number(size))
        except ValueError:
            raise LayoutError("Invalid partition offset or size") from None
        alignment = 0x10000 if kind == "app" else 0x1000
        if (
            part.offset < TABLE_OFFSET + TABLE_SIZE
            or part.offset % alignment
            or part.size <= 0
            or part.size % 0x1000
            or part.offset + part.size > FLASH_SIZE
        ):
            raise LayoutError("Partition alignment or flash bounds are invalid")
        partitions[name] = part
    end = TABLE_OFFSET + TABLE_SIZE
    for part in sorted(partitions.values(), key=lambda item: item.offset):
        if part.offset < end:
            raise LayoutError("Partitions overlap")
        end = part.offset + part.size
    for name, shape in {
        "nvs": ("data", "nvs"),
        "otadata": ("data", "ota"),
        "ota_0": ("app", "ota_0"),
        "ota_1": ("app", "ota_1"),
        "cajui": ("data", "nvs"),
    }.items():
        if name not in partitions or partitions[name][1:3] != shape:
            raise LayoutError(f"Missing or incompatible {name} partition")
    first, storage = partitions["ota_0"], partitions["cajui"]
    if (
        partitions["otadata"].offset + partitions["otadata"].size > first.offset
        or first.offset + first.size > storage.offset
    ):
        raise LayoutError("Merged USB image would overwrite persistent storage")
    return partitions


def image_limit(layout):
    return min(layout["ota_0"].size, layout["ota_1"].size)


def check_binary(data, layout):
    entries = b"".join(
        struct.pack(
            "<2sBBII16sI",
            b"\xaa\x50",
            TYPES[p.kind],
            SUBTYPES[p.kind, p.subtype],
            p.offset,
            p.size,
            p.name.encode(),
            0,
        )
        for p in layout.values()
    )
    expected = entries + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest()
    if (
        not len(expected) < len(data) <= TABLE_SIZE
        or data[: len(expected)] != expected
        or data[len(expected) :] != b"\xff" * (len(data) - len(expected))
    ):
        raise LayoutError("Built partition table does not match partitions.csv")
