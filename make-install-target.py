#!/usr/bin/env python3
"""Create the dedicated blank LatterOS installer test disk."""

from __future__ import annotations

import argparse
from pathlib import Path

TARGET_SIZE = 512 * 1024 * 1024


def create_sparse_image(path: Path, reset: bool) -> None:
    if path.exists() and not reset:
        if path.stat().st_size == TARGET_SIZE:
            print(f"Installation target already exists: {path} ({TARGET_SIZE // 1024 // 1024} MiB)")
            return
        raise SystemExit(
            f"Refusing to replace {path}: expected {TARGET_SIZE} bytes, "
            f"found {path.stat().st_size}. Use --reset to recreate it."
        )

    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("wb") as image:
        image.seek(TARGET_SIZE - 1)
        image.write(b"\0")

    print(f"Created blank installation target: {path} ({TARGET_SIZE // 1024 // 1024} MiB)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", nargs="?", default="latteros-install-target.img")
    parser.add_argument("--reset", action="store_true")
    arguments = parser.parse_args()
    create_sparse_image(Path(arguments.image), arguments.reset)


if __name__ == "__main__":
    main()
