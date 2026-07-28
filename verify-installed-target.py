#!/usr/bin/env python3
"""Verify a LatterOS Milestone 20D installation target."""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator

SECTOR_SIZE = 512


class VerificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def read_exact(handle: BinaryIO, offset: int, size: int) -> bytes:
    handle.seek(offset)
    data = handle.read(size)
    require(len(data) == size, f"short read at byte {offset}: wanted {size}, got {len(data)}")
    return data


@dataclass(frozen=True)
class Partition:
    index: int
    first_lba: int
    last_lba: int
    type_guid: bytes
    unique_guid: bytes
    name: str

    @property
    def sector_count(self) -> int:
        return self.last_lba - self.first_lba + 1


@dataclass(frozen=True)
class DirectoryEntry:
    name: str
    attributes: int
    first_cluster: int
    size: int

    @property
    def is_directory(self) -> bool:
        return bool(self.attributes & 0x10)


class Fat32Volume:
    def __init__(self, handle: BinaryIO, partition: Partition) -> None:
        self.handle = handle
        self.partition = partition
        boot = read_exact(handle, partition.first_lba * SECTOR_SIZE, SECTOR_SIZE)
        require(boot[510:512] == b"\x55\xaa", f"partition {partition.index}: invalid FAT boot signature")

        self.bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
        self.sectors_per_cluster = boot[13]
        self.reserved_sectors = struct.unpack_from("<H", boot, 14)[0]
        self.fat_count = boot[16]
        self.total_sectors = struct.unpack_from("<I", boot, 32)[0]
        self.fat_sectors = struct.unpack_from("<I", boot, 36)[0]
        self.root_cluster = struct.unpack_from("<I", boot, 44)[0]

        require(self.bytes_per_sector == SECTOR_SIZE, f"partition {partition.index}: unsupported sector size")
        require(self.sectors_per_cluster > 0, f"partition {partition.index}: invalid cluster size")
        require(self.fat_count >= 1, f"partition {partition.index}: no FAT tables")
        require(self.fat_sectors > 0, f"partition {partition.index}: invalid FAT size")
        require(self.root_cluster >= 2, f"partition {partition.index}: invalid root cluster")
        require(self.total_sectors <= partition.sector_count, f"partition {partition.index}: FAT geometry exceeds GPT entry")

        self.partition_offset = partition.first_lba * SECTOR_SIZE
        self.first_fat_sector = self.reserved_sectors
        self.first_data_sector = self.reserved_sectors + self.fat_count * self.fat_sectors
        self.cluster_bytes = self.bytes_per_sector * self.sectors_per_cluster

    def sector_offset(self, relative_sector: int) -> int:
        return self.partition_offset + relative_sector * self.bytes_per_sector

    def cluster_offset(self, cluster: int) -> int:
        require(cluster >= 2, f"partition {self.partition.index}: invalid cluster {cluster}")
        sector = self.first_data_sector + (cluster - 2) * self.sectors_per_cluster
        return self.sector_offset(sector)

    def fat_entry(self, cluster: int) -> int:
        offset = self.sector_offset(self.first_fat_sector) + cluster * 4
        value = struct.unpack("<I", read_exact(self.handle, offset, 4))[0]
        return value & 0x0FFFFFFF

    def cluster_chain(self, first_cluster: int) -> Iterator[int]:
        if first_cluster == 0:
            return
        seen: set[int] = set()
        cluster = first_cluster
        while 2 <= cluster < 0x0FFFFFF8:
            require(cluster not in seen, f"partition {self.partition.index}: FAT loop at cluster {cluster}")
            seen.add(cluster)
            yield cluster
            cluster = self.fat_entry(cluster)
        require(cluster >= 0x0FFFFFF8, f"partition {self.partition.index}: invalid FAT chain terminator 0x{cluster:x}")

    def read_chain(self, first_cluster: int, size: int | None = None) -> bytes:
        output = bytearray()
        for cluster in self.cluster_chain(first_cluster):
            output.extend(read_exact(self.handle, self.cluster_offset(cluster), self.cluster_bytes))
            if size is not None and len(output) >= size:
                break
        return bytes(output if size is None else output[:size])

    @staticmethod
    def decode_short_name(raw: bytes) -> str:
        base = raw[:8].decode("ascii", errors="replace").rstrip(" ")
        extension = raw[8:11].decode("ascii", errors="replace").rstrip(" ")
        return f"{base}.{extension}" if extension else base

    @staticmethod
    def decode_lfn(parts: dict[int, list[int]]) -> str | None:
        if not parts:
            return None
        units: list[int] = []
        for order in sorted(parts):
            units.extend(parts[order])
        trimmed: list[int] = []
        for unit in units:
            if unit == 0x0000:
                break
            if unit == 0xFFFF:
                continue
            trimmed.append(unit)
        if not trimmed:
            return None
        payload = b"".join(struct.pack("<H", unit) for unit in trimmed)
        return payload.decode("utf-16le", errors="strict")

    def directory_entries(self, cluster: int) -> list[DirectoryEntry]:
        data = self.read_chain(cluster)
        entries: list[DirectoryEntry] = []
        lfn_parts: dict[int, list[int]] = {}

        for offset in range(0, len(data), 32):
            entry = data[offset : offset + 32]
            if len(entry) < 32:
                break
            first = entry[0]
            if first == 0x00:
                break
            if first == 0xE5:
                lfn_parts.clear()
                continue
            attributes = entry[11]
            if attributes == 0x0F:
                order = entry[0] & 0x1F
                units = [
                    *struct.unpack_from("<5H", entry, 1),
                    *struct.unpack_from("<6H", entry, 14),
                    *struct.unpack_from("<2H", entry, 28),
                ]
                lfn_parts[order] = units
                continue
            if attributes & 0x08:
                lfn_parts.clear()
                continue

            name = self.decode_lfn(lfn_parts) or self.decode_short_name(entry[:11])
            lfn_parts.clear()
            high = struct.unpack_from("<H", entry, 20)[0]
            low = struct.unpack_from("<H", entry, 26)[0]
            first_cluster = (high << 16) | low
            size = struct.unpack_from("<I", entry, 28)[0]
            entries.append(DirectoryEntry(name, attributes, first_cluster, size))

        return entries

    def lookup(self, path: str) -> DirectoryEntry:
        components = [component for component in path.replace("\\", "/").split("/") if component]
        require(components, "empty FAT path")
        current_cluster = self.root_cluster
        current: DirectoryEntry | None = None

        for index, component in enumerate(components):
            matches = [
                entry
                for entry in self.directory_entries(current_cluster)
                if entry.name.casefold() == component.casefold()
            ]
            require(matches, f"partition {self.partition.index}: missing /{'/'.join(components[: index + 1])}")
            current = matches[0]
            if index + 1 < len(components):
                require(current.is_directory, f"partition {self.partition.index}: {current.name} is not a directory")
                current_cluster = current.first_cluster

        assert current is not None
        return current

    def read_file(self, path: str) -> bytes:
        entry = self.lookup(path)
        require(not entry.is_directory, f"partition {self.partition.index}: {path} is a directory")
        if entry.size == 0:
            return b""
        require(entry.first_cluster >= 2, f"partition {self.partition.index}: {path} has no data cluster")
        return self.read_chain(entry.first_cluster, entry.size)


def parse_gpt(handle: BinaryIO, image_size: int) -> list[Partition]:
    require(image_size >= 4 * SECTOR_SIZE, "image is too small")
    mbr = read_exact(handle, 0, SECTOR_SIZE)
    require(mbr[510:512] == b"\x55\xaa", "protective MBR signature missing")
    require(mbr[446 + 4] == 0xEE, "protective MBR partition type is not 0xEE")

    header = bytearray(read_exact(handle, SECTOR_SIZE, SECTOR_SIZE))
    require(header[:8] == b"EFI PART", "primary GPT signature missing")
    header_size = struct.unpack_from("<I", header, 12)[0]
    require(92 <= header_size <= SECTOR_SIZE, "invalid GPT header size")
    stored_header_crc = struct.unpack_from("<I", header, 16)[0]
    struct.pack_into("<I", header, 16, 0)
    require(zlib.crc32(header[:header_size]) & 0xFFFFFFFF == stored_header_crc, "primary GPT header CRC mismatch")

    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = struct.unpack_from("<I", header, 80)[0]
    entry_size = struct.unpack_from("<I", header, 84)[0]
    stored_entries_crc = struct.unpack_from("<I", header, 88)[0]
    require(entry_count >= 2 and entry_size >= 128, "invalid GPT entry geometry")
    entries_data = read_exact(handle, entries_lba * SECTOR_SIZE, entry_count * entry_size)
    require(zlib.crc32(entries_data) & 0xFFFFFFFF == stored_entries_crc, "GPT entry-array CRC mismatch")

    partitions: list[Partition] = []
    for index in range(entry_count):
        entry = entries_data[index * entry_size : (index + 1) * entry_size]
        type_guid = entry[:16]
        if type_guid == b"\0" * 16:
            continue
        unique_guid = entry[16:32]
        first_lba, last_lba = struct.unpack_from("<QQ", entry, 32)
        raw_name = entry[56:128]
        name = raw_name.decode("utf-16le", errors="ignore").split("\0", 1)[0]
        require(first_lba <= last_lba, f"GPT partition {index + 1} has reversed bounds")
        require(last_lba * SECTOR_SIZE < image_size, f"GPT partition {index + 1} exceeds image")
        partitions.append(Partition(index + 1, first_lba, last_lba, type_guid, unique_guid, name))

    require(len(partitions) >= 2, "expected EFI and system GPT partitions")
    return partitions


def verify_image(path: Path) -> None:
    require(path.exists(), f"image does not exist: {path}")
    image_size = path.stat().st_size

    with path.open("rb") as handle:
        partitions = parse_gpt(handle, image_size)
        esp = Fat32Volume(handle, partitions[0])
        system = Fat32Volume(handle, partitions[1])

        bootx64 = esp.read_file("/EFI/BOOT/BOOTX64.EFI")
        esp_config = esp.read_file("/boot/limine/limine.conf")
        adjacent_config = esp.read_file("/EFI/BOOT/limine.conf")
        esp_kernel = esp.read_file("/boot/kernel")
        esp_marker = esp.read_file("/INSTALL.TXT")

        system_kernel = system.read_file("/boot/kernel")
        system_config = system.read_file("/boot/limine/limine.conf")
        system_marker = system.read_file("/System/INSTALL.TXT")
        version = system.read_file("/System/VERSION.TXT")

        require(bootx64[:2] == b"MZ", "BOOTX64.EFI is not a PE/COFF executable")
        require(esp_kernel[:4] == b"\x7fELF", "EFI-partition kernel is not ELF")
        require(system_kernel[:4] == b"\x7fELF", "system-partition kernel is not ELF")
        require(esp_kernel == system_kernel, "EFI and system kernel copies differ")
        require(esp_config == adjacent_config == system_config, "installed Limine configurations differ")

        config_text = esp_config.decode("utf-8", errors="strict")
        require("protocol: limine" in config_text, "installed config has no Limine entry")
        require("timeout: 3" in config_text, "installed boot menu timeout is missing")
        require("quiet: yes" not in config_text.lower(), "installed boot menu is hidden by quiet mode")
        require("path: boot():/boot/kernel" in config_text, "installed config points at the wrong kernel")
        require("latteros-installer-kernel" in config_text, "installed config does not reload the kernel installer module")
        require("latteros-installer-bootx64" in config_text, "installed config does not reload the UEFI installer module")
        require("/LatterOS Safe Mode" in config_text, "installed config has no safe-mode entry")
        require("/LatterOS Recovery" in config_text, "installed config has no recovery entry")
        require("source=installed mode=normal" in config_text, "normal entry has no installed-system command line")
        require("source=installed mode=safe" in config_text, "safe-mode command line is missing")
        require("source=installed mode=recovery" in config_text, "recovery command line is missing")
        require("/LatterOS Hardware Test" in config_text, "installed config has no hardware-test entry")
        require("/LatterOS Compatibility Mode" in config_text, "installed config has no compatibility entry")
        require("mode=compatibility" in config_text, "installed config has no compatibility command line")
        require("/LatterOS VirtualBox Mode" in config_text, "installed config has no VirtualBox entry")
        require("source=installed mode=virtualbox" in config_text, "VirtualBox command line is missing")
        require("source=installed mode=hardware" in config_text, "hardware-test command line is missing")
        require(b"installation complete" in esp_marker.lower(), "EFI installation marker is incomplete")
        require(b"installation complete" in system_marker.lower(), "system installation marker is incomplete")
        require(b"milestone=20D" in version, "system version file is not Milestone 20D")
        require(b"version=0.20.3-rc1" in version, "system release version is missing")
        require(b"package-format=LPKGv1" in version, "LPKG package format metadata is missing")

        print(f"PASS protective MBR and GPT ({len(partitions)} partitions)")
        print(f"PASS EFI FAT32: {partitions[0].name or 'unnamed'} ({partitions[0].sector_count * SECTOR_SIZE // 1024 // 1024} MiB)")
        print(f"PASS BOOTX64.EFI: {len(bootx64)} bytes")
        print(f"PASS /boot/kernel: {len(esp_kernel)} bytes, CRC32={zlib.crc32(esp_kernel) & 0xFFFFFFFF:08x}")
        print("PASS Limine configuration in EFI/BOOT and boot/limine")
        print(f"PASS system FAT32: {partitions[1].name or 'unnamed'} ({partitions[1].sector_count * SECTOR_SIZE // 1024 // 1024} MiB)")
        print("PASS mirrored system kernel, installation marker, and version metadata")
        print("LatterOS Milestone 20D installation target verified")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", nargs="?", default="latteros-install-target.img")
    arguments = parser.parse_args()

    try:
        verify_image(Path(arguments.image))
    except (OSError, VerificationError, UnicodeError, struct.error) as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
