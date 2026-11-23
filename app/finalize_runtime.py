#!/usr/bin/env python3
"""Mark a disposable runtime install complete after the original updater exits."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import zlib

ENV_OFFSET = 0x00400000
ENV_BYTES = 0x00020000
SUCCESS_MARKERS = (
    "### /dev/mtd5 : images/rootfs.cramfs : Success",
    "### /dev/mtd7 : images/pdj.tar.gz : Success",
    "### /dev/mtd10 : images/gui.tar.gz : Success",
    "UPDATE successfully finished.",
)
CONTROLLER_FAILURE_MARKERS = (
    "MAIN firmware update failed.",
    "ERP APL ERROR",
    "MIXER SEND ERROR",
    "JOGLCD1L SEND ERROR",
    "JOGLCD2R SEND ERROR",
)


def rebind_bchaux(image: Path) -> None:
    """Keep QEMU's private BCH companion bound after an ENV-only rewrite."""
    sidecar = Path(str(image) + ".bchaux")
    if not sidecar.exists():
        return
    header = bytearray(sidecar.read_bytes()[:64])
    if len(header) != 64 or header[:8] != b"XZBCAUX1":
        raise ValueError("NAND BCH sidecar has an unexpected format")
    stat = image.stat()
    struct.pack_into("<Q", header, 16, stat.st_size)
    struct.pack_into("<Q", header, 24, stat.st_dev)
    struct.pack_into("<Q", header, 32, stat.st_ino)
    struct.pack_into("<q", header, 40, stat.st_mtime_ns // 1_000_000_000)
    struct.pack_into("<q", header, 48, stat.st_mtime_ns % 1_000_000_000)
    with sidecar.open("r+b") as stream:
        stream.write(header)
        stream.flush()
        os.fsync(stream.fileno())


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def normalized_environment(image: Path, boot_type: str) -> bytes:
    with image.open("rb") as stream:
        stream.seek(ENV_OFFSET + 4)
        raw = stream.read(ENV_BYTES - 4)
    values = []
    seen_boot_type = False
    for entry in raw.split(b"\0"):
        if not entry or b"=" not in entry:
            continue
        name, _ = entry.split(b"=", 1)
        if name == b"boot_type":
            values.append(f"boot_type={boot_type}".encode("ascii"))
            seen_boot_type = True
        else:
            values.append(entry)
    if not seen_boot_type:
        values.append(f"boot_type={boot_type}".encode("ascii"))
    payload = b"\0".join(values) + b"\0\0"
    if len(payload) > ENV_BYTES - 4:
        raise ValueError("installed U-Boot environment is too large")
    result = bytearray(b"\0" * ENV_BYTES)
    result[4:4 + len(payload)] = payload
    result[:4] = zlib.crc32(result[4:]).to_bytes(4, "little")
    return bytes(result)


def set_boot_type(image: Path, boot_type: str) -> None:
    environment = normalized_environment(image, boot_type)
    with image.open("r+b") as stream:
        stream.seek(ENV_OFFSET)
        stream.write(environment)
        stream.flush()
        os.fsync(stream.fileno())
    rebind_bchaux(image)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--serial-log", type=Path)
    parser.add_argument("--normal-after-recovery", action="store_true",
                        help="restore normal boot mode without interpreting updater results")
    parser.add_argument("--set-boot-type", choices=("normal", "update"),
                        help="set the next authentic U-Boot/initramfs boot mode")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    image = runtime / "nand.bin"
    if not image.is_file():
        raise SystemExit("runtime NAND is missing")
    if args.set_boot_type:
        if args.serial_log or args.normal_after_recovery:
            raise SystemExit("--set-boot-type cannot be combined with recovery finalization")
        set_boot_type(image, args.set_boot_type)
        print(f"runtime set to {args.set_boot_type} boot")
        return 0
    if not args.serial_log or not args.serial_log.is_file():
        raise SystemExit("recovery finalization requires its serial log")
    serial = args.serial_log.read_text(encoding="utf-8", errors="replace")
    missing = [marker for marker in SUCCESS_MARKERS if marker not in serial]
    controller_failures = [marker for marker in CONTROLLER_FAILURE_MARKERS if marker in serial]
    if not args.normal_after_recovery:
        if missing:
            raise SystemExit("original updater has not completed: " + "; ".join(missing))
        if controller_failures:
            raise SystemExit("original controller update did not complete: " +
                             "; ".join(controller_failures))
    set_boot_type(image, "normal")
    ready = {
        "schema": "xdj-xz-release-runtime/1",
        "installed_by": "original Pioneer recovery updater",
        "nand": {"path": str(image), "sha256": digest(image)},
        "serial_log": str(args.serial_log.resolve()),
        "completed_markers": [marker for marker in SUCCESS_MARKERS if marker not in missing],
        "controller_failures": controller_failures,
        "boot_type": "normal",
    }
    target = runtime / ("recovery-attempt.json" if args.normal_after_recovery else "ready.json")
    target.write_text(json.dumps(ready, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("runtime set to normal boot")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
