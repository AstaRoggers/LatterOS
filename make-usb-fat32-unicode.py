#!/usr/bin/env python3
"""Create a writable FAT32 image with Unicode VFAT names for LatterOS."""

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
TOTAL_SECTORS = 131072
RESERVED_SECTORS = 32
FAT_COUNT = 2
SECTORS_PER_CLUSTER = 1
SECTORS_PER_FAT = 1009
FIRST_DATA_SECTOR = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
ROOT_CLUSTER = 2
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6
VOLUME_LABEL = b"LATTEROS32 "


def cluster_sector(cluster: int) -> int:
    return FIRST_DATA_SECTOR + (cluster - 2) * SECTORS_PER_CLUSTER


def short_name(base: str, extension: str = "") -> bytes:
    name = base.upper().encode("ascii")
    ext = extension.upper().encode("ascii")

    if len(name) > 8 or len(ext) > 3:
        raise ValueError("invalid 8.3 alias")

    return name.ljust(8, b" ") + ext.ljust(3, b" ")


def short_checksum(alias: bytes) -> int:
    checksum = 0

    for value in alias:
        checksum = (
            ((checksum & 1) << 7) |
            (checksum >> 1)
        )
        checksum = (checksum + value) & 0xFF

    return checksum


def directory_entry(
    alias: bytes,
    attributes: int,
    cluster: int,
    size: int,
) -> bytes:
    entry = bytearray(32)
    entry[0:11] = alias
    entry[11] = attributes
    struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def lfn_entry(
    order: int,
    last: bool,
    checksum: int,
    characters: list[int],
) -> bytes:
    entry = bytearray(32)
    entry[0] = order | (0x40 if last else 0)
    entry[11] = 0x0F
    entry[12] = 0
    entry[13] = checksum
    struct.pack_into("<H", entry, 26, 0)

    offsets = [
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30,
    ]

    for offset, character in zip(
        offsets,
        characters,
        strict=True,
    ):
        struct.pack_into("<H", entry, offset, character)

    return bytes(entry)


def lfn_entries(long_name: str, alias: bytes) -> list[bytes]:
    encoded = long_name.encode("utf-16le")

    units = [
        encoded[index] |
        (encoded[index + 1] << 8)
        for index in range(0, len(encoded), 2)
    ]

    units.append(0)
    entry_count = math.ceil(len(units) / 13)
    units += [0xFFFF] * (entry_count * 13 - len(units))

    checksum = short_checksum(alias)
    entries: list[bytes] = []

    for order in range(entry_count, 0, -1):
        start = (order - 1) * 13
        entries.append(
            lfn_entry(
                order,
                order == entry_count,
                checksum,
                units[start : start + 13],
            )
        )

    return entries


def append_entry(
    directory: bytearray,
    offset: int,
    entry: bytes,
) -> int:
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
        directory_entry(
            alias,
            attributes,
            cluster,
            size,
        ),
    )


def write_cluster(
    image: bytearray,
    cluster: int,
    payload: bytes,
) -> None:
    if len(payload) > SECTOR_SIZE * SECTORS_PER_CLUSTER:
        raise ValueError("payload exceeds one test cluster")

    offset = cluster_sector(cluster) * SECTOR_SIZE
    image[offset : offset + len(payload)] = payload


def write_boot_sector(image: bytearray) -> None:
    boot = bytearray(SECTOR_SIZE)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"LATTEROS"
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COUNT
    struct.pack_into("<H", boot, 17, 0)
    struct.pack_into("<H", boot, 19, 0)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, 0)
    struct.pack_into("<H", boot, 24, 32)
    struct.pack_into("<H", boot, 26, 64)
    struct.pack_into("<I", boot, 28, 0)
    struct.pack_into("<I", boot, 32, TOTAL_SECTORS)
    struct.pack_into("<I", boot, 36, SECTORS_PER_FAT)
    struct.pack_into("<H", boot, 40, 0)
    struct.pack_into("<H", boot, 42, 0)
    struct.pack_into("<I", boot, 44, ROOT_CLUSTER)
    struct.pack_into("<H", boot, 48, FSINFO_SECTOR)
    struct.pack_into("<H", boot, 50, BACKUP_BOOT_SECTOR)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, 0x4C4F5332)
    boot[71:82] = VOLUME_LABEL
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"

    image[0:SECTOR_SIZE] = boot

    backup_offset = BACKUP_BOOT_SECTOR * SECTOR_SIZE
    image[backup_offset : backup_offset + SECTOR_SIZE] = boot


def write_fsinfo(image: bytearray, used_clusters: int) -> None:
    cluster_count = TOTAL_SECTORS - FIRST_DATA_SECTOR
    free_clusters = cluster_count - used_clusters

    fsinfo = bytearray(SECTOR_SIZE)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, free_clusters)
    struct.pack_into("<I", fsinfo, 492, ROOT_CLUSTER + used_clusters)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    primary = FSINFO_SECTOR * SECTOR_SIZE
    image[primary : primary + SECTOR_SIZE] = fsinfo

    backup_sector = BACKUP_BOOT_SECTOR + FSINFO_SECTOR
    backup = backup_sector * SECTOR_SIZE
    image[backup : backup + SECTOR_SIZE] = fsinfo


def main() -> None:
    output = Path(
        sys.argv[1]
        if len(sys.argv) > 1
        else "latteros-usb-complete.img"
    )

    image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)
    write_boot_sector(image)

    readme = (
        "Welcome to the LatterOS FAT32 USB drive!\r\n"
        "\r\n"
        "This volume is writable and uses UTF-8/VFAT names.\r\n"
    ).encode("utf-8")

    cafe = (
        "Unicode filename: café notes.txt\r\n"
        "FAT32 write support is active.\r\n"
    ).encode("utf-8")

    georgian = (
        "გამარჯობა გურამ!\r\n"
        "LatterOS decoded this VFAT filename from UTF-16.\r\n"
    ).encode("utf-8")

    guide = (
        "FAT32 features:\r\n"
        "[x] cluster allocation\r\n"
        "[x] directory growth\r\n"
        "[x] FSInfo updates\r\n"
        "[x] UTF-8 and UTF-16 VFAT conversion\r\n"
        "[x] Unicode create, rename, move, and delete\r\n"
    ).encode("utf-8")

    write_test = (
        "Initial FAT32 writable file.\r\n"
        "Use: write /media/usb/write.txt YOUR_TEXT\r\n"
    ).encode("utf-8")

    payloads = {
        3: readme,
        4: cafe,
        5: georgian,
        7: guide,
        8: write_test,
    }

    root = bytearray(SECTOR_SIZE)
    offset = 0

    offset = append_entry(
        root,
        offset,
        directory_entry(VOLUME_LABEL, 0x08, 0, 0),
    )

    offset = append_named_entry(
        root,
        offset,
        None,
        short_name("README", "TXT"),
        0x20,
        3,
        len(readme),
    )

    offset = append_named_entry(
        root,
        offset,
        "café notes.txt",
        short_name("CAFE~1", "TXT"),
        0x20,
        4,
        len(cafe),
    )

    offset = append_named_entry(
        root,
        offset,
        "გურამ.txt",
        short_name("GURAM~1", "TXT"),
        0x20,
        5,
        len(georgian),
    )

    offset = append_named_entry(
        root,
        offset,
        "Unicode Documents",
        short_name("UNICOD~1"),
        0x10,
        6,
        0,
    )

    offset = append_named_entry(
        root,
        offset,
        None,
        short_name("WRITE", "TXT"),
        0x20,
        8,
        len(write_test),
    )

    root[offset] = 0

    documents = bytearray(SECTOR_SIZE)
    offset = 0

    offset = append_entry(
        documents,
        offset,
        directory_entry(b".          ", 0x10, 6, 0),
    )

    offset = append_entry(
        documents,
        offset,
        directory_entry(b"..         ", 0x10, ROOT_CLUSTER, 0),
    )

    offset = append_named_entry(
        documents,
        offset,
        "FAT32 Unicode guide.txt",
        short_name("FAT32U~1", "TXT"),
        0x20,
        7,
        len(guide),
    )

    documents[offset] = 0

    used_clusters = 7
    write_fsinfo(image, used_clusters)

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    values = {
        0: 0x0FFFFFF8,
        1: 0xFFFFFFFF,
        2: 0x0FFFFFFF,
        3: 0x0FFFFFFF,
        4: 0x0FFFFFFF,
        5: 0x0FFFFFFF,
        6: 0x0FFFFFFF,
        7: 0x0FFFFFFF,
        8: 0x0FFFFFFF,
    }

    for cluster, value in values.items():
        struct.pack_into("<I", fat, cluster * 4, value)

    for fat_index in range(FAT_COUNT):
        sector = RESERVED_SECTORS + fat_index * SECTORS_PER_FAT
        offset = sector * SECTOR_SIZE
        image[offset : offset + len(fat)] = fat

    write_cluster(image, ROOT_CLUSTER, root)
    write_cluster(image, 3, readme)
    write_cluster(image, 4, cafe)
    write_cluster(image, 5, georgian)
    write_cluster(image, 6, documents)
    write_cluster(image, 7, guide)
    write_cluster(image, 8, write_test)

    output.write_bytes(image)

    print(
        f"Created {output} "
        f"({len(image) // (1024 * 1024)} MiB "
        "writable FAT32 + Unicode VFAT)"
    )


if __name__ == "__main__":
    main()
