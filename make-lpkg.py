#!/usr/bin/env python3
"""Build and verify deterministic LatterOS LPKG v1 application archives."""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

MAGIC = b"LPKGv1\0\0"
VERSION = 1
HEADER_SIZE = 64
HEADER = struct.Struct("<8s7I28s")
ENTRY = struct.Struct("<HHIII")
MAX_FILES = 32
MAX_MANIFEST = 1024
MAX_PATH = 239
MAX_ARCHIVE = 8 * 1024 * 1024
NAME_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,30}$")
EXECUTABLE_SUFFIXES = {".app", ".elf", ".bin"}


@dataclass(frozen=True)
class ArchiveFile:
    path: str
    data: bytes
    flags: int


def parse_manifest(text: str) -> dict[str, str]:
    values: dict[str, str] = {}

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"invalid manifest line: {raw_line!r}")
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()

    for required in ("name", "version"):
        if not values.get(required):
            raise ValueError(f"manifest is missing {required}=")

    if not NAME_PATTERN.fullmatch(values["name"]):
        raise ValueError("package name must use letters, digits, dot, dash, or underscore")

    return values


def normalized_manifest(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8")
    values = parse_manifest(text)
    ordered_keys = ("name", "version", "description", "depends", "entry")
    lines = [f"{key}={values.get(key, '')}" for key in ordered_keys]
    data = ("\n".join(lines) + "\n").encode("utf-8")

    if len(data) > MAX_MANIFEST:
        raise ValueError("manifest exceeds LPKG v1 limit")

    return data


def safe_relative_path(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    pure = PurePosixPath(relative)

    if (
        pure.is_absolute()
        or any(part in ("", ".", "..") for part in pure.parts)
        or "\\" in relative
        or len(relative.encode("utf-8")) > MAX_PATH
    ):
        raise ValueError(f"unsafe or oversized package path: {relative}")

    return relative


def collect_files(source: Path, manifest: Path) -> list[ArchiveFile]:
    files: list[ArchiveFile] = []

    for path in sorted(source.rglob("*"), key=lambda item: item.as_posix().casefold()):
        if not path.is_file() or path.resolve() == manifest.resolve():
            continue

        relative = safe_relative_path(path, source)
        data = path.read_bytes()
        flags = 1 if path.suffix.lower() in EXECUTABLE_SUFFIXES else 0
        files.append(ArchiveFile(relative, data, flags))

    if not files:
        raise ValueError("package source contains no payload files")
    if len(files) > MAX_FILES:
        raise ValueError(f"package contains more than {MAX_FILES} files")

    return files


def build_archive(manifest_path: Path, source: Path, output: Path) -> None:
    manifest = normalized_manifest(manifest_path)
    files = collect_files(source, manifest_path)

    payload = bytearray()
    table = bytearray()

    for archive_file in files:
        encoded_path = archive_file.path.encode("utf-8")
        data_offset = len(payload)
        payload.extend(archive_file.data)
        table.extend(
            ENTRY.pack(
                len(encoded_path),
                archive_file.flags,
                data_offset,
                len(archive_file.data),
                zlib.crc32(archive_file.data) & 0xFFFFFFFF,
            )
        )
        table.extend(encoded_path)

    body = manifest + bytes(table) + bytes(payload)
    header = HEADER.pack(
        MAGIC,
        HEADER_SIZE,
        VERSION,
        len(manifest),
        len(files),
        len(table),
        len(payload),
        zlib.crc32(body) & 0xFFFFFFFF,
        bytes(28),
    )
    archive = header + body

    if len(archive) > MAX_ARCHIVE:
        raise ValueError("archive exceeds LPKG v1 maximum size")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(archive)
    verify_archive(output)

    print(
        f"Created {output} ({len(files)} files, "
        f"{len(payload)} payload bytes, {len(archive)} archive bytes)"
    )


def verify_archive(path: Path) -> None:
    data = path.read_bytes()

    if len(data) < HEADER_SIZE or len(data) > MAX_ARCHIVE:
        raise ValueError("invalid archive size")

    (
        magic,
        header_size,
        version,
        manifest_size,
        file_count,
        table_size,
        payload_size,
        expected_crc,
        _,
    ) = HEADER.unpack_from(data, 0)

    if magic != MAGIC or header_size != HEADER_SIZE or version != VERSION:
        raise ValueError("invalid LPKG header")
    if not 1 <= file_count <= MAX_FILES:
        raise ValueError("invalid file count")
    if manifest_size > MAX_MANIFEST:
        raise ValueError("manifest too large")
    if HEADER_SIZE + manifest_size + table_size + payload_size != len(data):
        raise ValueError("archive section sizes do not match file size")

    body = data[HEADER_SIZE:]
    if zlib.crc32(body) & 0xFFFFFFFF != expected_crc:
        raise ValueError("archive CRC32 mismatch")

    manifest_end = HEADER_SIZE + manifest_size
    table_end = manifest_end + table_size
    payload_start = table_end
    manifest = data[HEADER_SIZE:manifest_end].decode("utf-8")
    parse_manifest(manifest)

    cursor = manifest_end
    seen: set[str] = set()

    for _ in range(file_count):
        if cursor + ENTRY.size > table_end:
            raise ValueError("truncated file table")
        path_length, _, offset, size, expected_file_crc = ENTRY.unpack_from(data, cursor)
        cursor += ENTRY.size
        if not 1 <= path_length <= MAX_PATH or cursor + path_length > table_end:
            raise ValueError("invalid package path length")
        relative = data[cursor : cursor + path_length].decode("utf-8")
        cursor += path_length
        pure = PurePosixPath(relative)
        if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
            raise ValueError("unsafe package path")
        if relative in seen:
            raise ValueError("duplicate package path")
        seen.add(relative)
        if offset > payload_size or size > payload_size - offset:
            raise ValueError("payload range outside archive")
        file_data = data[payload_start + offset : payload_start + offset + size]
        if zlib.crc32(file_data) & 0xFFFFFFFF != expected_file_crc:
            raise ValueError(f"payload CRC32 mismatch: {relative}")

    if cursor != table_end:
        raise ValueError("file table contains trailing bytes")

    print(f"PASS {path}: LPKG v1 archive verified")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", nargs="?")
    parser.add_argument("source", nargs="?")
    parser.add_argument("output", nargs="?")
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()

    try:
        if args.verify is not None:
            verify_archive(args.verify)
            return

        if not args.manifest or not args.source or not args.output:
            parser.error("MANIFEST SOURCE OUTPUT are required when not using --verify")

        build_archive(Path(args.manifest), Path(args.source), Path(args.output))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"LPKG error: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
