#!/usr/bin/env python3
"""Create and run supported Oracle VirtualBox profiles for LatterOS."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_NAME = "LatterOS 0.20.3 RC1"
DEFAULT_DIRECTORY = Path("dist/virtualbox")
DEFAULT_MEMORY_MIB = 2048
DEFAULT_CPUS = 4
DEFAULT_DISK_MIB = 1024


class VirtualBoxError(RuntimeError):
    pass


def locate_vboxmanage(explicit: str | None) -> Path:
    candidates: list[Path] = []

    if explicit:
        candidates.append(Path(explicit))

    environment = os.environ.get("VBOXMANAGE")
    if environment:
        candidates.append(Path(environment))

    for name in ("VBoxManage", "VBoxManage.exe"):
        resolved = shutil.which(name)
        if resolved:
            candidates.append(Path(resolved))

    for variable in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
        root = os.environ.get(variable)
        if root:
            candidates.append(
                Path(root) / "Oracle" / "VirtualBox" / "VBoxManage.exe"
            )

    seen: set[str] = set()
    for candidate in candidates:
        expanded = candidate.expanduser()
        key = str(expanded).lower()
        if key in seen:
            continue
        seen.add(key)
        if expanded.is_file():
            return expanded.resolve()

    raise VirtualBoxError(
        "VBoxManage was not found. Install Oracle VirtualBox or set "
        "VBOXMANAGE to the full VBoxManage executable path."
    )


class VBoxManage:
    def __init__(self, executable: Path, dry_run: bool) -> None:
        self.executable = executable
        self.dry_run = dry_run

    def run(
        self,
        *arguments: str,
        check: bool = True,
        capture: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        command = [str(self.executable), *arguments]
        print("+", subprocess.list2cmdline(command))

        if self.dry_run:
            return subprocess.CompletedProcess(command, 0, "", "")

        return subprocess.run(
            command,
            check=check,
            text=True,
            capture_output=capture,
        )

    def vm_exists(self, name: str) -> bool:
        if self.dry_run:
            return False
        result = self.run("showvminfo", name, check=False, capture=True)
        return result.returncode == 0

    def remove_vm(self, name: str) -> None:
        if not self.vm_exists(name):
            return
        self.run("controlvm", name, "poweroff", check=False)
        self.run("unregistervm", name, "--delete", check=False)


def require_file(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise VirtualBoxError(f"missing {description}: {resolved}")
    return resolved


def prepare_directory(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def configure_machine(
    vbox: VBoxManage,
    name: str,
    base_directory: Path,
    disk: Path,
    iso: Path | None,
    memory_mib: int,
    cpus: int,
) -> None:
    boot_one = "dvd" if iso is not None else "disk"
    boot_two = "disk" if iso is not None else "none"

    vbox.run(
        "createvm",
        "--name", name,
        "--ostype", "Other_64",
        "--basefolder", str(base_directory),
        "--register",
    )
    vbox.run(
        "modifyvm", name,
        "--memory", str(memory_mib),
        "--cpus", str(cpus),
        "--ioapic", "on",
        "--firmware", "efi",
        "--chipset", "ich9",
        "--graphicscontroller", "vmsvga",
        "--vram", "128",
        "--accelerate3d", "off",
        "--mouse", "ps2",
        "--keyboard", "ps2",
        "--rtcuseutc", "on",
        "--boot1", boot_one,
        "--boot2", boot_two,
        "--boot3", "none",
        "--boot4", "none",
        "--nic1", "none",
    )
    vbox.run(
        "setextradata",
        name,
        "VBoxInternal2/EfiGraphicsResolution",
        "1280x800",
    )
    vbox.run(
        "storagectl", name,
        "--name", "LatterOS SATA",
        "--add", "sata",
        "--controller", "IntelAhci",
        "--portcount", "4",
        "--hostiocache", "on",
        "--bootable", "on",
    )
    vbox.run(
        "storageattach", name,
        "--storagectl", "LatterOS SATA",
        "--port", "0",
        "--device", "0",
        "--type", "hdd",
        "--medium", str(disk),
    )

    if iso is not None:
        vbox.run(
            "storageattach", name,
            "--storagectl", "LatterOS SATA",
            "--port", "1",
            "--device", "0",
            "--type", "dvddrive",
            "--medium", str(iso),
        )


def create_installer_profile(
    vbox: VBoxManage,
    args: argparse.Namespace,
) -> None:
    iso = require_file(args.iso, "LatterOS installer ISO")
    directory = prepare_directory(args.directory)
    disk = directory / "latteros-virtualbox-install.vdi"

    if disk.exists() and not vbox.dry_run:
        disk.unlink()

    vbox.run(
        "createmedium", "disk",
        "--filename", str(disk),
        "--size", str(args.disk_mib),
        "--format", "VDI",
    )
    configure_machine(
        vbox,
        args.name,
        directory,
        disk,
        iso,
        args.memory,
        args.cpus,
    )


def create_installed_profile(
    vbox: VBoxManage,
    args: argparse.Namespace,
) -> None:
    image = require_file(args.image, "installed LatterOS raw image")
    directory = prepare_directory(args.directory)
    disk = directory / "latteros-installed.vdi"

    if disk.exists() and not vbox.dry_run:
        disk.unlink()

    vbox.run(
        "convertfromraw",
        str(image),
        str(disk),
        "--format", "VDI",
    )
    configure_machine(
        vbox,
        args.name,
        directory,
        disk,
        None,
        args.memory,
        args.cpus,
    )


def add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--name", default=DEFAULT_NAME)
    parser.add_argument("--directory", type=Path, default=DEFAULT_DIRECTORY)
    parser.add_argument("--memory", type=int, default=DEFAULT_MEMORY_MIB)
    parser.add_argument("--cpus", type=int, default=DEFAULT_CPUS)
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--start", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--vboxmanage")


def validate_resources(args: argparse.Namespace) -> None:
    if args.memory < 512 or args.memory > 65536:
        raise VirtualBoxError("memory must be between 512 and 65536 MiB")
    if args.cpus < 1 or args.cpus > 64:
        raise VirtualBoxError("CPU count must be between 1 and 64")
    if hasattr(args, "disk_mib") and args.disk_mib < 512:
        raise VirtualBoxError("installer disk must be at least 512 MiB")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Create supported Oracle VirtualBox profiles for LatterOS."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    installer = subparsers.add_parser(
        "installer",
        help="create an EFI VM that boots the LatterOS installer ISO",
    )
    add_common_arguments(installer)
    installer.add_argument("--iso", type=Path, default=Path("template-x86_64.iso"))
    installer.add_argument("--disk-mib", type=int, default=DEFAULT_DISK_MIB)

    installed = subparsers.add_parser(
        "installed",
        help="convert and boot the installed raw disk image",
    )
    add_common_arguments(installed)
    installed.add_argument(
        "--image",
        type=Path,
        default=Path("latteros-install-target.img"),
    )

    status = subparsers.add_parser("status", help="show the VM configuration")
    status.add_argument("--name", default=DEFAULT_NAME)
    status.add_argument("--dry-run", action="store_true")
    status.add_argument("--vboxmanage")

    clean = subparsers.add_parser("clean", help="remove the registered LatterOS VM")
    clean.add_argument("--name", default=DEFAULT_NAME)
    clean.add_argument("--directory", type=Path, default=DEFAULT_DIRECTORY)
    clean.add_argument("--dry-run", action="store_true")
    clean.add_argument("--vboxmanage")

    args = parser.parse_args()

    try:
        executable = locate_vboxmanage(args.vboxmanage)
        vbox = VBoxManage(executable, args.dry_run)

        if args.command == "status":
            vbox.run("showvminfo", args.name)
            return

        if args.command == "clean":
            vbox.remove_vm(args.name)
            directory = args.directory.expanduser().resolve()
            if directory.exists() and not args.dry_run:
                shutil.rmtree(directory)
            print(f"VirtualBox profile removed: {args.name}")
            return

        validate_resources(args)

        if vbox.vm_exists(args.name):
            if not args.replace:
                raise VirtualBoxError(
                    f"VM already exists: {args.name}. Use --replace to recreate it."
                )
            vbox.remove_vm(args.name)

        if args.command == "installer":
            create_installer_profile(vbox, args)
        else:
            create_installed_profile(vbox, args)

        print(f"VirtualBox profile ready: {args.name}")
        print("Firmware: EFI")
        print("Storage: Intel AHCI")
        print("Graphics: VMSVGA through the EFI framebuffer")
        print("Input: PS/2 keyboard and mouse")
        print("Resolution: 1280x800")

        if args.start:
            vbox.run("startvm", args.name, "--type", "gui")
    except (OSError, subprocess.CalledProcessError, VirtualBoxError) as error:
        print(f"VirtualBox error: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
