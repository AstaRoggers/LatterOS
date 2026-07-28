#!/usr/bin/env python3
"""Create versioned LatterOS ISO, installed-disk, recovery, and checksum artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ARCHITECTURE = "x86_64"
MILESTONE = "20D"
CHANNEL = "rc"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_file(path: Path, description: str) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"missing {description}: {path}")


def command_path(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise FileNotFoundError(f"required command is unavailable: {name}")
    return resolved


def resolve_limine_executable(limine_directory: Path) -> Path:
    candidates = (
        limine_directory / "limine",
        limine_directory / "limine.exe",
    )

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    searched = ", ".join(str(candidate) for candidate in candidates)
    raise FileNotFoundError(
        f"missing Limine host utility; searched: {searched}"
    )


def normalized_timestamp() -> int:
    value = os.environ.get("SOURCE_DATE_EPOCH")
    if value is None:
        return int(datetime.now(timezone.utc).timestamp())
    try:
        return int(value)
    except ValueError as error:
        raise ValueError("SOURCE_DATE_EPOCH must be an integer") from error


def set_timestamp(path: Path, timestamp: int) -> None:
    times = (timestamp, timestamp)

    try:
        os.utime(path, times, follow_symlinks=False)
    except (NotImplementedError, TypeError):
        # Windows/MSYS2 Python may not expose follow_symlinks for utime.
        # Release trees contain regular files and directories only, so the
        # ordinary call is equivalent on those hosts.
        os.utime(path, times)


def normalize_tree(root: Path, timestamp: int) -> None:
    for path in sorted(root.rglob("*")):
        set_timestamp(path, timestamp)
    set_timestamp(root, timestamp)


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(command, check=True)


def msys_tool_path(path: Path) -> str:
    """Return a path form understood by MSYS tools launched by Windows Python."""
    resolved = path.resolve()

    if os.name != "nt":
        return str(resolved)

    cygpath = shutil.which("cygpath")
    if cygpath is None:
        return str(resolved)

    completed = subprocess.run(
        [cygpath, "-u", str(resolved)],
        check=True,
        capture_output=True,
        text=True,
    )
    converted = completed.stdout.strip()

    if not converted:
        raise RuntimeError(f"cygpath returned an empty path for {resolved}")

    return converted


def build_recovery_iso(
    output: Path,
    kernel: Path,
    recovery_config: Path,
    limine_directory: Path,
    timestamp: int,
) -> None:
    required = {
        "limine-bios.sys": limine_directory / "limine-bios.sys",
        "limine-bios-cd.bin": limine_directory / "limine-bios-cd.bin",
        "limine-uefi-cd.bin": limine_directory / "limine-uefi-cd.bin",
        "BOOTX64.EFI": limine_directory / "BOOTX64.EFI",
        "BOOTIA32.EFI": limine_directory / "BOOTIA32.EFI",
    }
    for description, path in required.items():
        require_file(path, description)

    limine_executable = resolve_limine_executable(
        limine_directory
    )
    xorriso = command_path("xorriso")

    with tempfile.TemporaryDirectory(prefix="latteros-recovery-") as temporary:
        root = Path(temporary) / "root"
        (root / "boot" / "limine").mkdir(parents=True)
        (root / "EFI" / "BOOT").mkdir(parents=True)
        shutil.copy2(kernel, root / "boot" / "kernel")
        shutil.copy2(recovery_config, root / "boot" / "limine" / "limine.conf")
        shutil.copy2(required["limine-bios.sys"], root / "boot" / "limine")
        shutil.copy2(required["limine-bios-cd.bin"], root / "boot" / "limine")
        shutil.copy2(required["limine-uefi-cd.bin"], root / "boot" / "limine")
        shutil.copy2(required["BOOTX64.EFI"], root / "EFI" / "BOOT")
        shutil.copy2(required["BOOTIA32.EFI"], root / "EFI" / "BOOT")
        normalize_tree(root, timestamp)

        output.parent.mkdir(parents=True, exist_ok=True)
        run([
            xorriso,
            "-as", "mkisofs",
            "-R", "-r", "-J",
            "-V", "LATTEROS_REC",
            "-b", "boot/limine/limine-bios-cd.bin",
            "-no-emul-boot",
            "-boot-load-size", "4",
            "-boot-info-table",
            "-hfsplus",
            "-apm-block-size", "2048",
            "--efi-boot", "boot/limine/limine-uefi-cd.bin",
            "-efi-boot-part",
            "--efi-boot-image",
            "--protective-msdos-label",
            msys_tool_path(root),
            "-o", msys_tool_path(output),
        ])
        run([str(limine_executable), "bios-install", str(output)])


def copy_artifact(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version-file", type=Path, default=Path("release-version.txt"))
    parser.add_argument("--iso", type=Path, default=Path("template-x86_64.iso"))
    parser.add_argument("--installed", type=Path, default=Path("latteros-install-target.img"))
    parser.add_argument("--kernel", type=Path, default=Path("kernel/bin-x86_64/kernel"))
    parser.add_argument("--package", type=Path, default=Path("dist/packages/hello-1.0.0.lpkg"))
    parser.add_argument("--recovery-config", type=Path, default=Path("limine-recovery.conf"))
    parser.add_argument("--limine-directory", type=Path, default=Path("limine-binary"))
    parser.add_argument("--virtualbox-builder", type=Path, default=Path("make-virtualbox.py"))
    parser.add_argument("--virtualbox-readme", type=Path, default=Path("VIRTUALBOX.txt"))
    parser.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()

    try:
        version = args.version_file.read_text(encoding="utf-8").strip()
        if not version or any(character in version for character in "/\\"):
            raise ValueError("release version is invalid")

        for path, description in (
            (args.iso, "bootable ISO"),
            (args.installed, "installed disk image"),
            (args.kernel, "kernel"),
            (args.package, "demo package"),
            (args.recovery_config, "recovery configuration"),
            (args.virtualbox_builder, "VirtualBox builder"),
            (args.virtualbox_readme, "VirtualBox guide"),
        ):
            require_file(path, description)

        timestamp = normalized_timestamp()
        release_name = f"LatterOS-{version}-{ARCHITECTURE}"
        release_directory = args.output / release_name
        release_directory.mkdir(parents=True, exist_ok=True)

        normal_iso = release_directory / f"{release_name}.iso"
        installed_image = release_directory / f"{release_name}-installed.img"
        recovery_iso = release_directory / f"{release_name}-recovery.iso"
        demo_package = release_directory / args.package.name
        virtualbox_builder = release_directory / args.virtualbox_builder.name
        virtualbox_readme = release_directory / args.virtualbox_readme.name

        copy_artifact(args.iso, normal_iso)
        copy_artifact(args.installed, installed_image)
        copy_artifact(args.package, demo_package)
        copy_artifact(args.virtualbox_builder, virtualbox_builder)
        copy_artifact(args.virtualbox_readme, virtualbox_readme)
        build_recovery_iso(
            recovery_iso,
            args.kernel,
            args.recovery_config,
            args.limine_directory,
            timestamp,
        )

        artifacts = [
            normal_iso,
            installed_image,
            recovery_iso,
            demo_package,
            virtualbox_builder,
            virtualbox_readme,
        ]
        manifest_files = []
        for artifact in artifacts:
            manifest_files.append({
                "name": artifact.name,
                "size": artifact.stat().st_size,
                "sha256": sha256(artifact),
            })

        build_time = datetime.fromtimestamp(timestamp, timezone.utc).isoformat().replace("+00:00", "Z")
        manifest = {
            "name": "LatterOS",
            "version": version,
            "milestone": MILESTONE,
            "channel": CHANNEL,
            "architecture": ARCHITECTURE,
            "source_date_epoch": timestamp,
            "build_time_utc": build_time,
            "artifacts": manifest_files,
        }
        manifest_path = release_directory / "release-manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )

        notes_path = release_directory / "RELEASE-NOTES.txt"
        notes_path.write_text(
            f"LatterOS {version}\n"
            f"Milestone {MILESTONE} ({CHANNEL})\n\n"
            "Artifacts:\n"
            "- Bootable installer ISO with normal, Safe Mode, Recovery, Hardware Test, Compatibility, and VirtualBox entries\n"
            "- Installed UEFI disk image\n"
            "- Recovery-first ISO\n"
            "- LPKG v1 demo application package\n"
            "- Oracle VirtualBox VM builder and supported-profile guide\n\n"
            "This release candidate adds VirtualBox EFI/AHCI/VMSVGA/PS2 profiles, hypervisor detection, and persistent boot-health reporting.\n",
            encoding="utf-8",
            newline="\n",
        )

        checksum_targets = artifacts + [manifest_path, notes_path]
        sums_path = release_directory / "SHA256SUMS"
        sums_path.write_text(
            "".join(f"{sha256(path)}  {path.name}\n" for path in checksum_targets),
            encoding="ascii",
            newline="\n",
        )

        for path in release_directory.iterdir():
            set_timestamp(path, timestamp)

        print(f"LatterOS release created: {release_directory}")
        print(f"Manifest: {manifest_path}")
        print(f"Checksums: {sums_path}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Release error: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
