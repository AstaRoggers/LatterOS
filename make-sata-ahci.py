#!/usr/bin/env python3
"""Create a partitioned FAT16 SATA test disk for the LatterOS AHCI driver."""

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
DISK_SECTORS = 131072  # 64 MiB
PARTITION_START = 2048
PARTITION_SECTORS = DISK_SECTORS - PARTITION_START
SECTORS_PER_CLUSTER = 2
RESERVED_SECTORS = 1
FAT_COUNT = 2
SECTORS_PER_FAT = 256
ROOT_ENTRIES = 512
ROOT_DIRECTORY_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
FIRST_ROOT_SECTOR = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
FIRST_DATA_SECTOR = FIRST_ROOT_SECTOR + ROOT_DIRECTORY_SECTORS
VOLUME_LABEL = b"LATTEROSSAT"


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


def lfn_entry(order: int, last: bool, checksum: int, chars: list[int]) -> bytes:
    entry = bytearray(32)
    entry[0] = order | (0x40 if last else 0)
    entry[11] = 0x0F
    entry[13] = checksum
    offsets = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    for offset, character in zip(offsets, chars, strict=True):
        struct.pack_into("<H", entry, offset, character)
    return bytes(entry)


def lfn_entries(long_name: str, alias: bytes) -> list[bytes]:
    encoded = long_name.encode("utf-16le")
    units = [
        encoded[index] | (encoded[index + 1] << 8)
        for index in range(0, len(encoded), 2)
    ]
    units.append(0)
    count = math.ceil(len(units) / 13)
    units.extend([0xFFFF] * (count * 13 - len(units)))
    checksum = short_checksum(alias)
    result: list[bytes] = []
    for order in range(count, 0, -1):
        start = (order - 1) * 13
        result.append(
            lfn_entry(
                order,
                order == count,
                checksum,
                units[start : start + 13],
            )
        )
    return result


def append_entry(directory: bytearray, offset: int, entry: bytes) -> int:
    directory[offset : offset + 32] = entry
    return offset + 32


def append_named(
    directory: bytearray,
    offset: int,
    long_name: str | None,
    alias: bytes,
    attributes: int,
    cluster: int,
    size: int,
) -> int:
    if long_name:
        for entry in lfn_entries(long_name, alias):
            offset = append_entry(directory, offset, entry)
    return append_entry(
        directory,
        offset,
        directory_entry(alias, attributes, cluster, size),
    )


def partition_offset(sector: int) -> int:
    return (PARTITION_START + sector) * SECTOR_SIZE


def write_cluster(image: bytearray, cluster: int, payload: bytes) -> None:
    cluster_bytes = SECTORS_PER_CLUSTER * SECTOR_SIZE
    if len(payload) > cluster_bytes:
        raise ValueError("payload exceeds one cluster")
    sector = FIRST_DATA_SECTOR + (cluster - 2) * SECTORS_PER_CLUSTER
    offset = partition_offset(sector)
    image[offset : offset + len(payload)] = payload


def main() -> None:
    output = Path(sys.argv[1] if len(sys.argv) > 1 else "latteros-sata-ahci.img")
    image = bytearray(DISK_SECTORS * SECTOR_SIZE)

    # Master Boot Record with one FAT16 LBA partition.
    mbr = bytearray(SECTOR_SIZE)
    mbr[0:16] = b"LATTEROS-AHCI\0\0\0"
    entry = 446
    mbr[entry] = 0x00
    mbr[entry + 4] = 0x0E
    struct.pack_into("<I", mbr, entry + 8, PARTITION_START)
    struct.pack_into("<I", mbr, entry + 12, PARTITION_SECTORS)
    mbr[510:512] = b"\x55\xAA"
    image[0:SECTOR_SIZE] = mbr

    # Non-filesystem marker outside the partition for raw AHCI reads.
    marker = b"LATTEROS-AHCI-SATA-DISK\r\n"
    image[SECTOR_SIZE : SECTOR_SIZE + len(marker)] = marker

    boot = bytearray(SECTOR_SIZE)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"LATTEROS"
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, 0)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, SECTORS_PER_FAT)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 16)
    struct.pack_into("<I", boot, 28, PARTITION_START)
    struct.pack_into("<I", boot, 32, PARTITION_SECTORS)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x41484349)
    boot[43:54] = VOLUME_LABEL
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    start = partition_offset(0)
    image[start : start + SECTOR_SIZE] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    allocated = [0xFFF8, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF]
    for cluster, value in enumerate(allocated):
        struct.pack_into("<H", fat, cluster * 2, value)

    for fat_index in range(FAT_COUNT):
        sector = RESERVED_SECTORS + fat_index * SECTORS_PER_FAT
        offset = partition_offset(sector)
        image[offset : offset + len(fat)] = fat

    readme = (
        "LatterOS mounted this FAT16 partition through AHCI DMA.\r\n"
        "Path: /media/sata\r\n"
    ).encode("ascii")
    driver_notes = (
        "Storage path: PCI -> AHCI -> SATA -> MBR partition -> FAT16 -> VFS\r\n"
        "READ DMA EXT and WRITE DMA EXT are active.\r\n"
    ).encode("ascii")
    write_test = (
        "This file is writable through the LatterOS AHCI driver.\r\n"
    ).encode("ascii")
    checklist = (
        "[x] PCI AHCI discovery\r\n"
        "[x] HBA memory mapping\r\n"
        "[x] SATA IDENTIFY\r\n"
        "[x] DMA reads and writes\r\n"
        "[x] MBR partition discovery\r\n"
        "[x] FAT16 mounting\r\n"
    ).encode("ascii")

    root = bytearray(ROOT_DIRECTORY_SECTORS * SECTOR_SIZE)
    offset = 0
    offset = append_entry(root, offset, directory_entry(VOLUME_LABEL, 0x08, 0, 0))
    offset = append_named(
        root,
        offset,
        "sata readme.txt",
        short_name("SATARE~1", "TXT"),
        0x20,
        2,
        len(readme),
    )
    offset = append_named(root, offset, None, short_name("DRIVERS"), 0x10, 3, 0)
    offset = append_named(root, offset, None, short_name("WRITE", "TXT"), 0x20, 5, len(write_test))
    root[offset] = 0

    drivers = bytearray(SECTORS_PER_CLUSTER * SECTOR_SIZE)
    offset = 0
    offset = append_entry(drivers, offset, directory_entry(b".          ", 0x10, 3, 0))
    offset = append_entry(drivers, offset, directory_entry(b"..         ", 0x10, 0, 0))
    offset = append_named(
        drivers,
        offset,
        "ahci driver notes.txt",
        short_name("AHCIDR~1", "TXT"),
        0x20,
        4,
        len(driver_notes),
    )
    offset = append_named(
        drivers,
        offset,
        "storage checklist.txt",
        short_name("STORAG~1", "TXT"),
        0x20,
        6,
        len(checklist),
    )
    drivers[offset] = 0

    root_offset = partition_offset(FIRST_ROOT_SECTOR)
    image[root_offset : root_offset + len(root)] = root

    write_cluster(image, 2, readme)
    write_cluster(image, 3, drivers)
    write_cluster(image, 4, driver_notes)
    write_cluster(image, 5, write_test)
    write_cluster(image, 6, checklist)

    output.write_bytes(image)
    print(
        f"Created {output} ({len(image) // (1024 * 1024)} MiB, "
        f"MBR + writable FAT16 partition)"
    )


if __name__ == "__main__":
    main()
