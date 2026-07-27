#!/usr/bin/env python3
"""Create a GPT-partitioned FAT16 NVMe test disk for LatterOS."""

from __future__ import annotations

import binascii
import math
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
DISK_SECTORS = 262144  # 128 MiB
PRIMARY_HEADER_LBA = 1
PRIMARY_ENTRIES_LBA = 2
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_SECTORS = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE // SECTOR_SIZE
BACKUP_HEADER_LBA = DISK_SECTORS - 1
BACKUP_ENTRIES_LBA = BACKUP_HEADER_LBA - GPT_ENTRY_SECTORS
FIRST_USABLE_LBA = 2048
LAST_USABLE_LBA = BACKUP_ENTRIES_LBA - 1
PARTITION_START = FIRST_USABLE_LBA
PARTITION_END = LAST_USABLE_LBA
PARTITION_SECTORS = PARTITION_END - PARTITION_START + 1

SECTORS_PER_CLUSTER = 4
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 512
ROOT_DIRECTORY_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
VOLUME_LABEL = b"LATTEROSNVM"

# Microsoft Basic Data Partition GUID, in GPT on-disk byte order.
BASIC_DATA_TYPE_GUID = bytes.fromhex("A2A0D0EBE5B9334487C068B6B72699C7")
DISK_GUID = bytes.fromhex("78563412341278569ABCDEF012345678")
PARTITION_GUID = bytes.fromhex("2143658721436587A1B2C3D4E5F60718")


def calculate_fat_sectors() -> int:
    fat_sectors = 1
    while True:
        data_sectors = (
            PARTITION_SECTORS
            - RESERVED_SECTORS
            - ROOT_DIRECTORY_SECTORS
            - FAT_COUNT * fat_sectors
        )
        clusters = data_sectors // SECTORS_PER_CLUSTER
        required = math.ceil((clusters + 2) * 2 / SECTOR_SIZE)
        if required == fat_sectors:
            return fat_sectors
        fat_sectors = required


SECTORS_PER_FAT = calculate_fat_sectors()
FIRST_ROOT_SECTOR = RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT
FIRST_DATA_SECTOR = FIRST_ROOT_SECTOR + ROOT_DIRECTORY_SECTORS


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


def gpt_header(
    current_lba: int,
    backup_lba: int,
    entries_lba: int,
    entries_crc: int,
) -> bytes:
    header = bytearray(SECTOR_SIZE)
    struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0,
        b"EFI PART",
        0x00010000,
        92,
        0,
        0,
        current_lba,
        backup_lba,
        FIRST_USABLE_LBA,
        LAST_USABLE_LBA,
        DISK_GUID,
        entries_lba,
        GPT_ENTRY_COUNT,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    checksum = binascii.crc32(header[:92]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, checksum)
    return bytes(header)


def main() -> None:
    output = Path(sys.argv[1] if len(sys.argv) > 1 else "latteros-nvme-gpt.img")
    image = bytearray(DISK_SECTORS * SECTOR_SIZE)

    # Protective MBR.
    mbr = bytearray(SECTOR_SIZE)
    entry = 446
    mbr[entry + 4] = 0xEE
    struct.pack_into("<I", mbr, entry + 8, 1)
    struct.pack_into("<I", mbr, entry + 12, min(DISK_SECTORS - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    image[0:SECTOR_SIZE] = mbr

    # Raw diagnostic marker outside the GPT partition.
    marker = b"LATTEROS-NVME-GPT-DISK\r\n"
    marker_offset = 128 * SECTOR_SIZE
    image[marker_offset : marker_offset + len(marker)] = marker

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    entries[0:16] = BASIC_DATA_TYPE_GUID
    entries[16:32] = PARTITION_GUID
    struct.pack_into("<QQQ", entries, 32, PARTITION_START, PARTITION_END, 0)
    name = "LatterOS NVMe Data".encode("utf-16le")
    entries[56 : 56 + len(name)] = name
    entries_crc = binascii.crc32(entries) & 0xFFFFFFFF

    primary_entries_offset = PRIMARY_ENTRIES_LBA * SECTOR_SIZE
    image[primary_entries_offset : primary_entries_offset + len(entries)] = entries
    image[PRIMARY_HEADER_LBA * SECTOR_SIZE : (PRIMARY_HEADER_LBA + 1) * SECTOR_SIZE] = gpt_header(
        PRIMARY_HEADER_LBA,
        BACKUP_HEADER_LBA,
        PRIMARY_ENTRIES_LBA,
        entries_crc,
    )

    backup_entries_offset = BACKUP_ENTRIES_LBA * SECTOR_SIZE
    image[backup_entries_offset : backup_entries_offset + len(entries)] = entries
    image[BACKUP_HEADER_LBA * SECTOR_SIZE : (BACKUP_HEADER_LBA + 1) * SECTOR_SIZE] = gpt_header(
        BACKUP_HEADER_LBA,
        PRIMARY_HEADER_LBA,
        BACKUP_ENTRIES_LBA,
        entries_crc,
    )

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
    struct.pack_into("<I", boot, 39, 0x4E564D45)
    boot[43:54] = VOLUME_LABEL
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    image[partition_offset(0) : partition_offset(0) + SECTOR_SIZE] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    allocated = [0xFFF8, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF]
    for cluster, value in enumerate(allocated):
        struct.pack_into("<H", fat, cluster * 2, value)

    for fat_index in range(FAT_COUNT):
        sector = RESERVED_SECTORS + fat_index * SECTORS_PER_FAT
        offset = partition_offset(sector)
        image[offset : offset + len(fat)] = fat

    readme = (
        "LatterOS mounted this GPT FAT16 partition through NVMe queues.\r\n"
        "Path: /media/nvme\r\n"
    ).encode("ascii")
    driver_notes = (
        "Storage path: PCI -> NVMe -> namespace -> GPT -> FAT16 -> VFS\r\n"
        "Admin and I/O submission/completion queues are active.\r\n"
    ).encode("ascii")
    write_test = (
        "This file is writable through the LatterOS NVMe driver.\r\n"
    ).encode("ascii")
    checklist = (
        "[x] PCI NVMe discovery\r\n"
        "[x] Admin queue setup\r\n"
        "[x] Identify controller and namespace\r\n"
        "[x] I/O queue creation\r\n"
        "[x] NVMe reads, writes, and flush\r\n"
        "[x] GPT partition discovery\r\n"
        "[x] FAT16 mounting\r\n"
    ).encode("ascii")

    root = bytearray(ROOT_DIRECTORY_SECTORS * SECTOR_SIZE)
    offset = 0
    offset = append_entry(root, offset, directory_entry(VOLUME_LABEL, 0x08, 0, 0))
    offset = append_named(
        root,
        offset,
        "nvme readme.txt",
        short_name("NVMERE~1", "TXT"),
        0x20,
        2,
        len(readme),
    )
    offset = append_named(root, offset, None, short_name("TOOLS"), 0x10, 3, 0)
    offset = append_named(root, offset, None, short_name("WRITE", "TXT"), 0x20, 5, len(write_test))
    root[offset] = 0

    tools = bytearray(SECTORS_PER_CLUSTER * SECTOR_SIZE)
    offset = 0
    offset = append_entry(tools, offset, directory_entry(b".          ", 0x10, 3, 0))
    offset = append_entry(tools, offset, directory_entry(b"..         ", 0x10, 0, 0))
    offset = append_named(
        tools,
        offset,
        "nvme driver notes.txt",
        short_name("NVMEDR~1", "TXT"),
        0x20,
        4,
        len(driver_notes),
    )
    offset = append_named(
        tools,
        offset,
        "storage checklist.txt",
        short_name("STORAG~1", "TXT"),
        0x20,
        6,
        len(checklist),
    )
    tools[offset] = 0

    root_offset = partition_offset(FIRST_ROOT_SECTOR)
    image[root_offset : root_offset + len(root)] = root

    write_cluster(image, 2, readme)
    write_cluster(image, 3, tools)
    write_cluster(image, 4, driver_notes)
    write_cluster(image, 5, write_test)
    write_cluster(image, 6, checklist)

    output.write_bytes(image)
    print(
        f"Created {output} ({len(image) // (1024 * 1024)} MiB, "
        f"GPT + writable FAT16 partition, FAT sectors={SECTORS_PER_FAT})"
    )


if __name__ == "__main__":
    main()
