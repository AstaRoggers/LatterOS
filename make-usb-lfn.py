#!/usr/bin/env python3
"""Create the deterministic FAT16 + VFAT long-name test image for LatterOS."""

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
TOTAL_SECTORS = 32768
RESERVED_SECTORS = 1
FAT_COUNT = 2
SECTORS_PER_FAT = 128
ROOT_ENTRIES = 512
ROOT_DIRECTORY_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
FIRST_ROOT_SECTOR = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
FIRST_DATA_SECTOR = FIRST_ROOT_SECTOR + ROOT_DIRECTORY_SECTORS
VOLUME_LABEL = b"LATTEROSUSB"


def short_name(name: str, extension: str = "") -> bytes:
    base = name.upper().encode("ascii")
    ext = extension.upper().encode("ascii")
    if len(base) > 8 or len(ext) > 3:
        raise ValueError("invalid 8.3 alias")
    return base.ljust(8, b" ") + ext.ljust(3, b" ")


def directory_entry(name: bytes, attributes: int, cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = attributes
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def short_checksum(name: bytes) -> int:
    checksum = 0
    for value in name:
        checksum = (((checksum & 1) << 7) | (checksum >> 1))
        checksum = (checksum + value) & 0xFF
    return checksum


def lfn_entry(order: int, last: bool, checksum: int, characters: list[int]) -> bytes:
    entry = bytearray(32)
    entry[0] = order | (0x40 if last else 0)
    entry[11] = 0x0F
    entry[12] = 0
    entry[13] = checksum
    struct.pack_into("<H", entry, 26, 0)

    offsets = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    for offset, character in zip(offsets, characters, strict=True):
        struct.pack_into("<H", entry, offset, character)

    return bytes(entry)


def lfn_entries(long_name: str, alias: bytes) -> list[bytes]:
    utf16 = list(long_name.encode("utf-16le"))
    code_units = [
        utf16[index] | (utf16[index + 1] << 8)
        for index in range(0, len(utf16), 2)
    ]
    code_units.append(0x0000)

    entry_count = math.ceil(len(code_units) / 13)
    padded = code_units + [0xFFFF] * (entry_count * 13 - len(code_units))
    checksum = short_checksum(alias)

    entries: list[bytes] = []
    for order in range(entry_count, 0, -1):
        start = (order - 1) * 13
        entries.append(
            lfn_entry(
                order,
                order == entry_count,
                checksum,
                padded[start : start + 13],
            )
        )
    return entries


def append_entry(directory: bytearray, offset: int, entry: bytes) -> int:
    directory[offset : offset + 32] = entry
    return offset + 32


def append_named_entry(
    directory: bytearray,
    offset: int,
    long_name: str | None,
    alias: bytes,
    attributes: int,
    cluster: int,
    size: int,
) -> int:
    if long_name is not None:
        for entry in lfn_entries(long_name, alias):
            offset = append_entry(directory, offset, entry)
    return append_entry(
        directory,
        offset,
        directory_entry(alias, attributes, cluster, size),
    )


def write_cluster(image: bytearray, cluster: int, payload: bytes) -> None:
    sector = FIRST_DATA_SECTOR + cluster - 2
    offset = sector * SECTOR_SIZE
    if len(payload) > SECTOR_SIZE:
        raise ValueError("test payload exceeds one cluster")
    image[offset : offset + len(payload)] = payload


def main() -> None:
    output = Path(sys.argv[1] if len(sys.argv) > 1 else "latteros-usb-lfn.img")
    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    boot = bytearray(SECTOR_SIZE)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"LATTEROS"
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = 1
    struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, TOTAL_SECTORS)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, SECTORS_PER_FAT)
    struct.pack_into("<H", boot, 24, 32)
    struct.pack_into("<H", boot, 26, 64)
    struct.pack_into("<I", boot, 28, 0)
    struct.pack_into("<I", boot, 32, 0)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x4C464E31)
    boot[43:54] = VOLUME_LABEL
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    image[0:SECTOR_SIZE] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    for cluster, value in enumerate(
        [
            0xFFF8,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
            0xFFFF,
        ]
    ):
        struct.pack_into("<H", fat, cluster * 2, value)

    for fat_index in range(FAT_COUNT):
        sector = RESERVED_SECTORS + fat_index * SECTORS_PER_FAT
        offset = sector * SECTOR_SIZE
        image[offset : offset + len(fat)] = fat

    readme = (
        "Welcome to the LatterOS USB drive!\r\n"
        "\r\n"
        "This volume now contains VFAT long filenames.\r\n"
        "Try: ls /media/usb\r\n"
    ).encode("ascii")

    hello = "Hello from a USB flash drive mounted by LatterOS.\r\n".encode("ascii")
    system = (
        "Filesystem: FAT16 read-only\r\n"
        "Names: 8.3 aliases plus VFAT Long File Names\r\n"
        "Mount point: /media/usb\r\n"
    ).encode("ascii")
    guide = (
        "LatterOS VFAT long filename support is working.\r\n"
        "This file uses multiple long-name directory entries.\r\n"
    ).encode("ascii")
    notes = (
        "USB -> SCSI -> block device -> FAT16 -> VFS\r\n"
        "The visible filename came from VFAT LFN entries.\r\n"
    ).encode("ascii")
    checklist = (
        "[x] USB enumeration\r\n"
        "[x] USB mass storage\r\n"
        "[x] FAT16 mount\r\n"
        "[x] Long filenames\r\n"
        "[ ] Writable FAT\r\n"
    ).encode("ascii")

    root = bytearray(ROOT_DIRECTORY_SECTORS * SECTOR_SIZE)
    offset = 0
    offset = append_entry(root, offset, directory_entry(VOLUME_LABEL, 0x08, 0, 0))
    offset = append_named_entry(root, offset, None, short_name("README", "TXT"), 0x20, 2, len(readme))
    offset = append_named_entry(root, offset, None, short_name("DOCS"), 0x10, 3, 0)
    offset = append_named_entry(
        root,
        offset,
        "latteros welcome guide.txt",
        short_name("LATTER~1", "TXT"),
        0x20,
        6,
        len(guide),
    )
    offset = append_named_entry(
        root,
        offset,
        "project documents",
        short_name("PROJEC~1"),
        0x10,
        7,
        0,
    )
    root[offset] = 0

    docs = bytearray(SECTOR_SIZE)
    offset = 0
    offset = append_entry(docs, offset, directory_entry(b".          ", 0x10, 3, 0))
    offset = append_entry(docs, offset, directory_entry(b"..         ", 0x10, 0, 0))
    offset = append_named_entry(docs, offset, None, short_name("HELLO", "TXT"), 0x20, 4, len(hello))
    offset = append_named_entry(docs, offset, None, short_name("SYSTEM", "TXT"), 0x20, 5, len(system))
    docs[offset] = 0

    projects = bytearray(SECTOR_SIZE)
    offset = 0
    offset = append_entry(projects, offset, directory_entry(b".          ", 0x10, 7, 0))
    offset = append_entry(projects, offset, directory_entry(b"..         ", 0x10, 0, 0))
    offset = append_named_entry(
        projects,
        offset,
        "usb filesystem notes.txt",
        short_name("USBFIL~1", "TXT"),
        0x20,
        8,
        len(notes),
    )
    offset = append_named_entry(
        projects,
        offset,
        "release checklist.txt",
        short_name("RELEAS~1", "TXT"),
        0x20,
        9,
        len(checklist),
    )
    projects[offset] = 0

    root_offset = FIRST_ROOT_SECTOR * SECTOR_SIZE
    image[root_offset : root_offset + len(root)] = root

    write_cluster(image, 2, readme)
    write_cluster(image, 3, docs)
    write_cluster(image, 4, hello)
    write_cluster(image, 5, system)
    write_cluster(image, 6, guide)
    write_cluster(image, 7, projects)
    write_cluster(image, 8, notes)
    write_cluster(image, 9, checklist)

    output.write_bytes(image)
    print(f"Created {output} ({len(image) // (1024 * 1024)} MiB FAT16 + VFAT LFN)")


if __name__ == "__main__":
    main()
