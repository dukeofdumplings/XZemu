#!/usr/bin/env python3
"""Create a disposable virtual-NAND bootstrap from an original XDJXZ.UPD."""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ModuleNotFoundError as error:
    raise SystemExit("The bundled Python runtime needs its cryptography module") from error

CHIP_BYTES = 0x10000000
ENV_OFFSET = 0x00400000
ENV_BYTES = 0x00100000
KERNEL_OFFSET = 0x00500000
ROOTFS_OFFSET = 0x01900000
SETTINGS_OFFSET = 0x03700000
BOOT_BYTES = 0x00300000
KERNEL_BYTES = 0x00A00000
ROOTFS_BYTES = 0x01E00000
SETTINGS_BYTES = 0x00A00000
RECOVERY_MEDIA_BYTES = 80 * 1024 * 1024
FAT_SECTOR_BYTES = 512
EXPECTED_UPD_SHA256 = "be0d8709cc859774f8a5785fa702ae909a3c78896ea95982935a2965376dc0a5"
EXPECTED_SETTINGS_SHA256 = "480f716b222c3b4c1f414cdf1cf9664a15c25d7b74a56bf8558fe5e3abcbb0b9"
MEMBERS = {
    "/images/u-boot.bin.nand": "u-boot.bin.nand",
    "/images/uImage": "uImage",
    "/images/rootfs.cramfs": "rootfs.cramfs",
    "/images/pdj.tar.gz": "pdj.tar.gz",
    "/images/gui.tar.gz": "gui.tar.gz",
    "/images/settings.tar.gz": "settings.tar.gz",
    "/images/fw_env.txt": "fw_env.txt",
}


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def decrypt(data: bytes, key: bytes) -> bytes:
    if len(data) % 512:
        raise ValueError("UPD encrypted region must have full 512-byte sectors")
    plain = bytearray(len(data))
    for offset in range(0, len(data), 512):
        iv = struct.pack("<I", offset // 512) + bytes(12)
        transform = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        plain[offset:offset + 512] = transform.update(data[offset:offset + 512]) + transform.finalize()
    return bytes(plain)


def decrypted_updater_iso(upd: Path, key_file: Path) -> bytes:
    if digest(upd) != EXPECTED_UPD_SHA256:
        raise ValueError("unsupported UPD identity; expected the verified XDJ-XZ 1.26 updater")
    key = key_file.read_bytes().splitlines()[0][:31].ljust(32, b"\0")
    encrypted = upd.read_bytes()
    plain = decrypt(encrypted[:len(encrypted) // 512 * 512], key)
    if plain[32769:32774] != b"CD001":
        raise ValueError("UPD did not decrypt to ISO9660; check the supplied boot key")
    return plain


def _updater_tree(upd: Path, key_file: Path):
    """Return a temporary extraction of the original decrypted ISO.

    macOS ships ``bsdtar`` with ISO9660/Rock Ridge support.  It avoids making
    users install a Python ISO parser merely to build a recovery runtime.
    """
    temporary = tempfile.TemporaryDirectory(prefix="xdj-xz-upd-")
    root = Path(temporary.name)
    iso = root / "update.iso"
    iso.write_bytes(decrypted_updater_iso(upd, key_file))
    content = root / "content"
    content.mkdir()
    try:
        subprocess.run(["bsdtar", "-xf", str(iso), "-C", str(content)],
                       check=True, capture_output=True, text=True)
    except FileNotFoundError as error:
        temporary.cleanup()
        raise RuntimeError("bsdtar is required to unpack the original updater") from error
    except subprocess.CalledProcessError as error:
        temporary.cleanup()
        raise RuntimeError(f"could not unpack decrypted updater: {error.stderr.strip()}") from error
    return temporary, content


def extract(upd: Path, key_file: Path, firmware: Path) -> dict[str, Path]:
    temporary, root = _updater_tree(upd, key_file)
    result: dict[str, Path] = {}
    try:
        for source, name in MEMBERS.items():
            candidate = root / source.lstrip("/")
            if not candidate.is_file():
                raise ValueError(f"original updater lacks required member {source}")
            target = firmware / name
            target.write_bytes(candidate.read_bytes())
            result[name] = target
    finally:
        temporary.cleanup()
    return result


@dataclass
class FATNode:
    name: str
    data: bytes | None = None
    children: dict[str, "FATNode"] = field(default_factory=dict)
    short_name: bytes = b""
    cluster: int = 0
    cluster_count: int = 0

    @property
    def is_directory(self) -> bool:
        return self.data is None


def _fat_short_name(name: str, used: set[bytes]) -> bytes:
    """Produce a stable FAT alias while retaining the original name through LFN."""
    encoded = name.encode("ascii", "strict")
    if "." in name:
        stem, extension = name.rsplit(".", 1)
    else:
        stem, extension = name, ""
    clean = lambda value: "".join(ch if ch.isalnum() else "_" for ch in value.upper())
    stem, extension = clean(stem), clean(extension)[:3]
    candidate = (stem[:8].ljust(8) + extension.ljust(3)).encode("ascii")
    if len(stem) <= 8 and len(extension) <= 3 and candidate not in used:
        used.add(candidate)
        return candidate
    for number in range(1, 100000):
        suffix = f"~{number}"
        alias = (stem[:8 - len(suffix)] + suffix).ljust(8) + extension.ljust(3)
        candidate = alias.encode("ascii")
        if candidate not in used:
            used.add(candidate)
            return candidate
    raise ValueError(f"unable to create FAT alias for {name}")


def _fat_short_checksum(short_name: bytes) -> int:
    value = 0
    for byte in short_name:
        value = ((value & 1) << 7) + (value >> 1) + byte
        value &= 0xff
    return value


def _fat_lfn_entries(name: str, short_name: bytes) -> list[bytes]:
    words = list(struct.unpack("<" + "H" * (len(name.encode("utf-16le")) // 2),
                               name.encode("utf-16le")))
    words.append(0)
    while len(words) % 13:
        words.append(0xffff)
    chunks = [words[index:index + 13] for index in range(0, len(words), 13)]
    entries: list[bytes] = []
    checksum = _fat_short_checksum(short_name)
    for index in range(len(chunks), 0, -1):
        entry = bytearray(32)
        entry[0] = index | (0x40 if index == len(chunks) else 0)
        entry[11] = 0x0f
        entry[13] = checksum
        struct.pack_into("<H", entry, 26, 0)
        chunk = chunks[index - 1]
        positions = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
        for position, codepoint in zip(positions, chunk):
            struct.pack_into("<H", entry, position, codepoint)
        entries.append(bytes(entry))
    return entries


def _fat_short_entry(short_name: bytes, directory: bool, cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[:11] = short_name
    entry[11] = 0x10 if directory else 0x20
    struct.pack_into("<H", entry, 20, cluster >> 16)
    struct.pack_into("<H", entry, 26, cluster & 0xffff)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def _fat_directory_bytes(node: FATNode, parent: FATNode | None) -> bytes:
    entries: list[bytes] = []
    if parent is not None:
        entries.append(_fat_short_entry(b".          ", True, node.cluster, 0))
        entries.append(_fat_short_entry(b"..         ", True, parent.cluster, 0))
    for name, child in sorted(node.children.items(), key=lambda pair: pair[0].casefold()):
        entries.extend(_fat_lfn_entries(name, child.short_name))
        entries.append(_fat_short_entry(child.short_name, child.is_directory,
                                        child.cluster, 0 if child.is_directory else len(child.data)))
    return b"".join(entries)


def _fat_set_chain(table: list[int], first: int, count: int, end: int) -> None:
    for index in range(count):
        table[first + index] = end if index + 1 == count else first + index + 1


def build_recovery_media(upd: Path, output: Path) -> dict[str, object]:
    """Place the untouched encrypted UPD on the original-style FAT medium.

    The recovery initramfs mounts a FAT USB drive containing ``XDJXZ.UPD``
    then performs its own decryption and ISO handling.  It does *not* expect
    the ISO's expanded files to be placed directly on the USB filesystem.
    """
    if output.exists():
        raise FileExistsError(f"refusing to overwrite recovery media: {output}")
    if digest(upd) != EXPECTED_UPD_SHA256:
        raise ValueError("unsupported UPD identity; expected the verified XDJ-XZ 1.26 updater")
    root = FATNode(name="")
    payload = upd.read_bytes()
    root.children["XDJXZ.UPD"] = FATNode(name="XDJXZ.UPD", data=payload)
    files = {"/XDJXZ.UPD": payload}

    def aliases(node: FATNode) -> None:
        used: set[bytes] = set()
        for child in node.children.values():
            child.short_name = _fat_short_name(child.name, used)
            if child.is_directory:
                aliases(child)
    aliases(root)

    partition_start = 1
    partition_sectors = RECOVERY_MEDIA_BYTES // FAT_SECTOR_BYTES - partition_start
    # The independently verified recovery USB is a 80 MiB FAT16 volume.  The
    # 3.0.101 updater kernel can drive FAT32 too, but its known-good updater
    # path has this FAT16 geometry; keeping it avoids turning the bootstrap
    # into a filesystem-compatibility experiment.
    reserved, fats, sectors_per_cluster = 1, 2, 4
    root_entries = 512
    root_sectors = (root_entries * 32 + FAT_SECTOR_BYTES - 1) // FAT_SECTOR_BYTES
    fat_sectors = 1
    while True:
        clusters = ((partition_sectors - reserved - fats * fat_sectors - root_sectors)
                    // sectors_per_cluster)
        next_size = (clusters + 2) * 2
        next_sectors = (next_size + FAT_SECTOR_BYTES - 1) // FAT_SECTOR_BYTES
        if next_sectors == fat_sectors:
            break
        fat_sectors = next_sectors
    cluster_bytes = sectors_per_cluster * FAT_SECTOR_BYTES
    table = [0] * (clusters + 2)
    table[0], table[1] = 0xfff8, 0xffff
    next_cluster = 2

    def allocate(node: FATNode, parent: FATNode | None) -> None:
        nonlocal next_cluster
        if node.is_directory:
            length = len(_fat_directory_bytes(node, parent))
        else:
            length = len(node.data)
        node.cluster_count = max(1, (length + cluster_bytes - 1) // cluster_bytes)
        if next_cluster + node.cluster_count > len(table):
            raise ValueError("original updater does not fit in the 80 MiB recovery medium")
        node.cluster = next_cluster
        _fat_set_chain(table, node.cluster, node.cluster_count, 0xffff)
        next_cluster += node.cluster_count
        if node.is_directory:
            for child in node.children.values():
                allocate(child, node)
    # FAT16's root is a fixed directory table, not a data-cluster chain.
    root.cluster, root.cluster_count = 0, 0
    for child in root.children.values():
        allocate(child, root)
    root_directory = _fat_directory_bytes(root, None)
    if len(root_directory) > root_entries * 32:
        raise ValueError("original updater has too many root entries for FAT16")

    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("xb") as image:
            image.truncate(RECOVERY_MEDIA_BYTES)
        with output.open("r+b") as image:
            mbr = bytearray(FAT_SECTOR_BYTES)
            mbr[0x1be:0x1ce] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0x0b,
                                             b"\xfe\xff\xff", partition_start, partition_sectors)
            mbr[510:512] = b"\x55\xaa"
            image.write(mbr)
            boot = bytearray(FAT_SECTOR_BYTES)
            boot[:3], boot[3:11] = b"\xeb\x3c\x90", b"XZEMUUPD"
            struct.pack_into("<H", boot, 11, FAT_SECTOR_BYTES)
            boot[13], boot[16] = sectors_per_cluster, fats
            struct.pack_into("<H", boot, 14, reserved)
            struct.pack_into("<H", boot, 17, root_entries)
            boot[21] = 0xf8
            struct.pack_into("<H", boot, 22, fat_sectors)
            struct.pack_into("<H", boot, 24, 63)
            struct.pack_into("<H", boot, 26, 255)
            struct.pack_into("<I", boot, 28, partition_start)
            struct.pack_into("<I", boot, 32, partition_sectors)
            boot[36], boot[38] = 0x80, 0x29
            struct.pack_into("<I", boot, 39, 0x585a4555)
            boot[43:54], boot[54:62] = b"XDJXZ UPD  ", b"FAT16   "
            boot[510:512] = b"\x55\xaa"
            image.seek(partition_start * FAT_SECTOR_BYTES)
            image.write(boot)
            fat = struct.pack("<" + "H" * len(table), *table)
            fat_offset = (partition_start + reserved) * FAT_SECTOR_BYTES
            for copy in range(fats):
                image.seek(fat_offset + copy * fat_sectors * FAT_SECTOR_BYTES)
                image.write(fat)
            root_offset = (partition_start + reserved + fats * fat_sectors) * FAT_SECTOR_BYTES
            image.seek(root_offset)
            image.write(root_directory)
            data_offset = root_offset + root_sectors * FAT_SECTOR_BYTES

            def write_node(node: FATNode, parent: FATNode | None) -> None:
                content = _fat_directory_bytes(node, parent) if node.is_directory else node.data
                image.seek(data_offset + (node.cluster - 2) * cluster_bytes)
                image.write(content)
                if node.is_directory:
                    for child in node.children.values():
                        write_node(child, node)
            for child in root.children.values():
                write_node(child, root)
    except Exception:
        output.unlink(missing_ok=True)
        raise
    return {"path": str(output.resolve()), "bytes": output.stat().st_size,
            "sha256": digest(output), "files": len(files),
            "original_members": {path: {"bytes": len(data),
                                          "sha256": hashlib.sha256(data).hexdigest()}
                                 for path, data in sorted(files.items())}}


def uboot_env(source: Path, *, display_bootstrap: bool, recovery_boot: bool) -> bytes:
    values: list[bytes] = []
    for line in source.read_text(encoding="utf-8", errors="strict").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        values.append(line.encode("ascii"))
    if recovery_boot:
        for index, entry in enumerate(values):
            if entry.startswith(b"boot_type="):
                # The supplied initramfs selects its original USB-update path
                # only for this exact value (or an ErP physical-key request).
                values[index] = b"boot_type=update"
                break
        else:
            values.append(b"boot_type=update")
    if display_bootstrap:
        for index, entry in enumerate(values):
            if entry.startswith(b"bootargs_base="):
                bootargs = entry.split(b"=", 1)[1]
                suffix = b" keep_bootcon rdinit=/bin/sh"
                if b"rdinit=" in bootargs:
                    raise ValueError("updater environment already supplies rdinit")
                values[index] = b"bootargs_base=" + bootargs + suffix
                break
        else:
            raise ValueError("updater environment lacks bootargs_base")
    if not any(item.startswith(b"ubifs_formatted=") for item in values):
        values.append(b"ubifs_formatted=true")
    payload = b"\0".join(values) + b"\0\0"
    if len(payload) > 128 * 1024 - 4:
        raise ValueError("UPD environment exceeds U-Boot's observed CRC scope")
    env = bytearray(b"\xff" * ENV_BYTES)
    # U-Boot reserves a 1 MiB NAND range for this environment, but the
    # Pioneer build CRCs its 128 KiB environment sector.  The supplied
    # fw_env.config confirms the latter; a full-range checksum makes U-Boot
    # silently select its default, normal-mode environment instead.
    env[4:128 * 1024] = b"\0" * (128 * 1024 - 4)
    env[4:4 + len(payload)] = payload
    env[:4] = zlib.crc32(env[4:128 * 1024]).to_bytes(4, "little")
    return bytes(env)


def copy_at(image: Path, source: Path, offset: int, maximum: int) -> None:
    if source.stat().st_size > maximum:
        raise ValueError(f"{source.name} exceeds its NAND partition")
    with image.open("r+b") as output, source.open("rb") as input_stream:
        output.seek(offset)
        while block := input_stream.read(1024 * 1024):
            output.write(block)


def verified_settings_seed(source: Path) -> Path:
    """Validate the exact 10 MiB main-area capture used for Pioneer settings.

    The updater contains an empty ``settings.tar.gz`` because the original
    device retains this UBI partition across updates.  A recovery-ready image
    needs the separately verified MTD6 main-area capture instead.  It is not
    physical OOB data; the virtual-NAND BCH contract deliberately derives its
    internal clean-page state at runtime.
    """
    if not source.is_file() or source.stat().st_size != SETTINGS_BYTES:
        raise ValueError("settings seed must be the verified 10 MiB MTD6 main-area image")
    if digest(source) != EXPECTED_SETTINGS_SHA256:
        raise ValueError("unsupported settings seed identity")
    media = source.read_bytes()
    if (media[:4] != b"UBI#" or media[10_227_728:10_227_736] != b"settings"
            or b"\x31\x18\x10\x06" not in media):
        raise ValueError("settings seed lacks the expected Pioneer UBI layout")
    return source


def initialize_image(output: Path, files: dict[str, Path], *, display_bootstrap: bool,
                     recovery_boot: bool, settings_seed: Path | None) -> None:
    with output.open("xb") as stream:
        block = b"\xff" * (1024 * 1024)
        for _ in range(CHIP_BYTES // len(block)):
            stream.write(block)
    copy_at(output, files["u-boot.bin.nand"], 0, BOOT_BYTES)
    copy_at(output, files["uImage"], KERNEL_OFFSET, KERNEL_BYTES)
    copy_at(output, files["rootfs.cramfs"], ROOTFS_OFFSET, ROOTFS_BYTES)
    if settings_seed:
        copy_at(output, settings_seed, SETTINGS_OFFSET, SETTINGS_BYTES)
    with output.open("r+b") as stream:
        stream.seek(ENV_OFFSET)
        stream.write(uboot_env(files["fw_env.txt"], display_bootstrap=display_bootstrap,
                               recovery_boot=recovery_boot))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--upd", required=True, type=Path)
    parser.add_argument("--key", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--settings-seed", type=Path,
                        help="optional verified 10 MiB MTD6 main-area capture")
    parser.add_argument("--display-bootstrap", choices=("on", "off"), default="off")
    args = parser.parse_args()
    if not args.upd.is_file() or not args.key.is_file():
        raise SystemExit("--upd and --key must name regular files")
    runtime = args.runtime.resolve()
    firmware = runtime / "firmware"
    image = runtime / "nand.bin"
    manifest = runtime / "bootstrap.json"
    recovery_media = runtime / "recovery.dmg"
    if image.exists() or manifest.exists():
        raise SystemExit("runtime already initialized; refuse to overwrite NAND")
    firmware.mkdir(parents=True, exist_ok=False)
    try:
        files = extract(args.upd, args.key, firmware)
        settings_seed = (verified_settings_seed(args.settings_seed)
                         if args.settings_seed else None)
        initialize_image(image, files, display_bootstrap=args.display_bootstrap == "on",
                         recovery_boot=True, settings_seed=settings_seed)
        media = build_recovery_media(args.upd, recovery_media)
        result = {
            "schema": "xdj-xz-release-bootstrap/1",
            "nand": {"path": str(image), "bytes": image.stat().st_size, "sha256": digest(image)},
            "firmware": {name: {"bytes": path.stat().st_size, "sha256": digest(path)} for name, path in files.items()},
            "settings_seed": ({"bytes": settings_seed.stat().st_size,
                               "sha256": digest(settings_seed)}
                              if settings_seed else None),
            "recovery_media": media,
            "pending_guest_provisioning": ["pdj", "gui"],
            "display_bootstrap": args.display_bootstrap == "on",
            "boot_type": "update",
        }
        manifest.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2, sort_keys=True))
    except Exception:
        recovery_media.unlink(missing_ok=True)
        if image.exists():
            image.unlink()
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())
