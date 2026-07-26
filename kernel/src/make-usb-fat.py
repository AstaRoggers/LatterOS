#!/usr/bin/env python3
"""Create the deterministic FAT16 USB test image used by LatterOS."""

from __future__ import annotations

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
    return name.upper().encode("ascii").ljust(8, b" ") + extension.upper().encode("ascii").ljust(3, b" ")


def directory_entry(name: bytes, attributes: int, cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = attributes
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def write_cluster(image: bytearray, cluster: int, payload: bytes) -> None:
    sector = FIRST_DATA_SECTOR + cluster - 2
    offset = sector * SECTOR_SIZE
    image[offset : offset + len(payload)] = payload


def main() -> None:
    output = Path(sys.argv[1] if len(sys.argv) > 1 else "latteros-usb-fat.img")
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
    struct.pack_into("<I", boot, 39, 0x4C415454)
    boot[43:54] = VOLUME_LABEL
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    image[0:SECTOR_SIZE] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    entries = [0xFFF8, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF]
    for index, value in enumerate(entries):
        struct.pack_into("<H", fat, index * 2, value)

    for fat_index in range(FAT_COUNT):
        sector = RESERVED_SECTORS + fat_index * SECTORS_PER_FAT
        offset = sector * SECTOR_SIZE
        image[offset : offset + len(fat)] = fat

    readme = (
        "Welcome to the LatterOS USB drive!\r\n"
        "\r\n"
        "This file is being read from a FAT16 filesystem through the\r\n"
        "USB Mass Storage, SCSI, block-device, FAT, and VFS layers.\r\n"
        "\r\n"
        "Try: ls /media/usb/docs\r\n"
        "     cat /media/usb/docs/hello.txt\r\n"
    ).encode("ascii")

    hello = (
        "Hello from a USB flash drive mounted by LatterOS.\r\n"
    ).encode("ascii")

    system = (
        "Filesystem: FAT16 (read-only)\r\n"
        "Mount point: /media/usb\r\n"
        "Transport: USB Bulk-Only + SCSI READ(10)\r\n"
    ).encode("ascii")

    root = bytearray(ROOT_DIRECTORY_SECTORS * SECTOR_SIZE)
    root[0:32] = directory_entry(VOLUME_LABEL, 0x08, 0, 0)
    root[32:64] = directory_entry(short_name("README", "TXT"), 0x20, 2, len(readme))
    root[64:96] = directory_entry(short_name("DOCS"), 0x10, 3, 0)
    root_offset = FIRST_ROOT_SECTOR * SECTOR_SIZE
    image[root_offset : root_offset + len(root)] = root

    docs = bytearray(SECTOR_SIZE)
    docs[0:32] = directory_entry(b".          ", 0x10, 3, 0)
    docs[32:64] = directory_entry(b"..         ", 0x10, 0, 0)
    docs[64:96] = directory_entry(short_name("HELLO", "TXT"), 0x20, 4, len(hello))
    docs[96:128] = directory_entry(short_name("SYSTEM", "TXT"), 0x20, 5, len(system))

    write_cluster(image, 2, readme)
    write_cluster(image, 3, docs)
    write_cluster(image, 4, hello)
    write_cluster(image, 5, system)

    output.write_bytes(image)
    print(f"Created {output} ({len(image) // (1024 * 1024)} MiB FAT16)")


if __name__ == "__main__":
    main()
