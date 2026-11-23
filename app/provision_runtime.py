#!/usr/bin/env python3
"""Guest-provision settings, PDJ, and GUI from a freshly extracted UPD."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import select
import shutil
import socket
import subprocess
import time
import zlib

ENV_OFFSET = 0x00400000
ENV_SCOPE = 128 * 1024
VOLUMES = ((6, "settings", 78), (7, "pdj", 62), (10, "gui", 313))
REQUIRED_GLOBALS = (
    "fsl-imx6.openvg-reset-diagnostic-enable=on",
    "fsl-imx6.gc2000-diagnostic-enable=on",
    "fsl-imx6.gc2000-diagnostic-reference-identity=on",
    "fsl-imx6.gc320-diagnostic-enable=on",
    "fsl-imx6.gc320-diagnostic-gc320-model=on",
    "fsl-imx6.ipu1-trace-diagnostic-enable=on",
    "fsl-imx6.ipu1-bg23-eof-diagnostic-enable=on",
    "fsl-imx6.ipu1-bg23-cpmem-scanout-diagnostic-enable=on",
)
PROVISION_MACHINE = (
    "sabrelite,xz-m16-erp=panel-first-reports-tempo-center,"
    "xz-mixer=synthetic-menu,xz-jog=synthetic-play-cue,"
    "xz-tusb4020bi=on,xz-rtl8309m=on"
)


def set_bootargs(nand: Path, replacement: bytes) -> str:
    """Update bootargs_base and return its prior value."""
    with nand.open("r+b") as stream:
        stream.seek(ENV_OFFSET)
        env = bytearray(stream.read(ENV_SCOPE))
        end = env[4:].find(b"\0\0")
        if len(env) != ENV_SCOPE or end < 0:
            raise ValueError("invalid U-Boot environment")
        entries = env[4:4 + end].split(b"\0")
        original = next((entry.split(b"=", 1)[1] for entry in entries
                         if entry.startswith(b"bootargs_base=")), None)
        if not original:
            raise ValueError("U-Boot bootargs_base missing")
        entries = [b"bootargs_base=" + replacement
                   if entry.startswith(b"bootargs_base=") else entry for entry in entries]
        payload = b"\0".join(entries) + b"\0\0"
        if len(payload) > ENV_SCOPE - 4:
            raise ValueError("overlong U-Boot environment")
        env[4:] = payload + bytes(ENV_SCOPE - 4 - len(payload))
        env[:4] = zlib.crc32(env[4:]).to_bytes(4, "little")
        stream.seek(ENV_OFFSET)
        stream.write(env)
    return original.decode("ascii")


def patch_rdinit(nand: Path) -> str:
    """Temporarily boot into the initramfs shell for guest provisioning."""
    with nand.open("rb") as stream:
        stream.seek(ENV_OFFSET + 4)
        payload = stream.read(ENV_SCOPE - 4)
    end = payload.find(b"\0\0")
    entries = payload[:end].split(b"\0") if end >= 0 else []
    original = next((entry.split(b"=", 1)[1] for entry in entries
                     if entry.startswith(b"bootargs_base=")), None)
    if not original or b"rdinit=" in original:
        raise ValueError("unsupported U-Boot bootargs_base")
    set_bootargs(nand, original + b" keep_bootcon rdinit=/bin/sh")
    return original.decode("ascii")


def report(stage: str) -> None:
    print(f"[XZemu] {stage}", flush=True)


def display_bootargs(original: str) -> str:
    """Return the verified U-Boot bootstrap used by the working GUI runtime.

    The original environment is retained verbatim except for the two explicit
    diagnostic boot arguments.  ``rdinit=/bin/sh`` gives serial_relay.py one
    initramfs shell prompt in which it sets the process-local DirectFB fallback
    before resuming the unmodified Pioneer ``/init``.
    """
    if "rdinit=" in original:
        raise ValueError("original bootargs unexpectedly already contain rdinit")
    suffix = " keep_bootcon rdinit=/bin/sh"
    return original if original.endswith(suffix) else original + suffix


def connect(path: Path, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        channel = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            channel.connect(str(path))
            return channel
        except OSError:
            channel.close()
            time.sleep(0.1)
    raise TimeoutError("guest serial endpoint unavailable")


def wait_for(channel: socket.socket, output: bytearray, marker: bytes, timeout: float,
             serial_log: Path) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([channel], [], [], min(0.5, deadline - time.monotonic()))
        if ready:
            payload = channel.recv(65536)
            if not payload:
                return False
            output.extend(payload)
            with serial_log.open("ab") as stream:
                stream.write(payload)
            if marker in output:
                return True
    return marker in output


def send(channel: socket.socket, script: str) -> None:
    for line in script.strip().splitlines():
        for byte in line.encode("ascii"):
            channel.sendall(bytes((byte,)))
            time.sleep(0.002)
        channel.sendall(b"\r")
        time.sleep(0.35)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--display-bootstrap", choices=("on", "off"), default="off",
                        help="retain the validated initramfs/DirectFB handoff in U-Boot ENV")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    nand, firmware, ready = runtime / "nand.bin", runtime / "firmware", runtime / "ready.json"
    if ready.exists():
        return 0
    if not args.qemu.is_file() or not nand.is_file():
        raise SystemExit("QEMU or initialized NAND missing")
    for _, label, _ in VOLUMES:
        if not (firmware / f"{label}.tar.gz").is_file():
            raise SystemExit(f"missing updater payload: {label}.tar.gz")
    source, serial = runtime / "provision-input", runtime / "provision.serial.sock"
    if source.exists() or serial.exists():
        raise SystemExit("interrupted provisioning artifacts exist; recreate this runtime")
    source.mkdir()
    for _, label, _ in VOLUMES:
        shutil.copy2(firmware / f"{label}.tar.gz", source / f"{label}.tar.gz")
    bootargs = patch_rdinit(nand)
    report("Starting temporary Pioneer provisioning boot…")
    command = [str(args.qemu), "-M", PROVISION_MACHINE, "-smp", "4", "-m", "1024", "-monitor", "none",
               "-nographic", "-serial", f"unix:{serial},server=on,wait=off"]
    command.extend(["-serial", "null"] * 4)
    command.extend(["-device", f"loader,file={firmware / 'u-boot.bin.nand'},addr=0x17800000,cpu-num=0,force-raw=on",
                    "-global", f"imx6-gpmi-reset.virtual-nand={nand}",
                    "-global", "fsl-imx6.nand-boot-mode=on",
                    "-global", "imx6-gpmi-reset.virtual-nand-ram-cache=on",
                    "-drive", f"file=fat:ro:{source},format=raw,if=none,readonly=on,id=provision",
                    "-device", "usb-storage,drive=provision,bus=usb-bus.1"])
    for value in REQUIRED_GLOBALS:
        command.extend(["-global", value])
    transcript, process = bytearray(), None
    serial_log = runtime / "provision.serial.log"
    serial_log.write_bytes(b"")
    try:
        with (runtime / "provision.log").open("xb") as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log)
            with connect(serial, 30) as channel:
                if not wait_for(channel, transcript, b"/ # ", 240, serial_log):
                    raise TimeoutError("initramfs shell unavailable")
                # BusyBox echoes every typed line.  Completion markers must be
                # emitted by a finished guest command, not matched in that
                # echo, otherwise the following UBI operation interleaves
                # with the operation still in progress.
                send(channel, "stty -echo")
                time.sleep(0.5)
                report("Preparing the original filesystem…")
                send(channel, """
mount -n -t proc proc /proc
mount -n -t sysfs sys /sys
mkdir -p /mnt/flash /mnt/rootfs
mount -t tmpfs -o size=96m tmpfs /mnt/flash
nanddump -q --omitoob --bb=skipbad --length=0x1400000 -f /mnt/flash/rootfs.cramfs /dev/mtd5
mount -t cramfs -o loop=/dev/loop3 /mnt/flash/rootfs.cramfs /mnt/rootfs
for dir in bin dev etc home lib mnt opt proc root sbin sys usr var; do cp -a /mnt/rootfs/$dir /mnt/flash; done
mkdir -p /mnt/flash/tmp /mnt/flash/media/usb1 /mnt/flash/media/usb2
chmod -R ug+wr /mnt/flash
exec /sbin/switch_root -c /dev/console /mnt/flash /bin/sh
""")
                time.sleep(3)
                send(channel, """
stty -echo
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc || true
mount -t sysfs sys /sys || true
/etc/rc.d/init.d/udev start || true
sleep 2
mkdir -p /mnt/usb /tmp/xz-seed /tmp/xz-work
mount -t vfat /dev/sda /mnt/usb || mount -t vfat /dev/sda1 /mnt/usb
mount -t tmpfs -o size=210m tmpfs /tmp
echo __XDJ_FACTORY_ROOT_READY__
""")
                if not wait_for(channel, transcript, b"__XDJ_FACTORY_ROOT_READY__", 150, serial_log):
                    raise TimeoutError("guest rootfs preparation failed")
                for mtd, label, lebs in VOLUMES:
                    report(f"Creating {label} storage…")
                    marker = f"__XDJ_FACTORY_{label.upper()}_DONE__"
                    send(channel, f"""
rm -rf /tmp/xz-seed /tmp/xz-work
mkdir -p /tmp/xz-seed /tmp/xz-work
tar zxf /mnt/usb/{label}.tar.gz -C /tmp/xz-seed
ubiformat /dev/mtd{mtd} -y
ubiattach /dev/ubi_ctrl -m {mtd} -d {mtd} -O 2048
ubimkvol /dev/ubi{mtd} -N {label} -m
mkfs.ubifs -r /tmp/xz-seed -m 2048 -e 126976 -c {lebs} -o /tmp/xz-work/{label}.ubifs
ubiupdatevol /dev/ubi{mtd}_0 /tmp/xz-work/{label}.ubifs
mount -t ubifs ubi{mtd}:{label} /tmp/xz-work
test -n "$(find /tmp/xz-work -type f -print | head -n 1)"
umount /tmp/xz-work
echo {marker}
""")
                    if not wait_for(channel, transcript, marker.encode(), 420, serial_log):
                        raise TimeoutError(f"{label} provisioning failed")
                report("Finalizing runtime…")
                final_bootargs = (display_bootargs(bootargs)
                                  if args.display_bootstrap == "on" else bootargs)
                send(channel, "fw_setenv bootargs_base '" + final_bootargs + "'\necho __XDJ_FACTORY_READY__")
                if not wait_for(channel, transcript, b"__XDJ_FACTORY_READY__", 120, serial_log):
                    raise TimeoutError("bootargs restoration failed")
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=20)
        serial.unlink(missing_ok=True)
        shutil.rmtree(source, ignore_errors=True)
        if b"__XDJ_FACTORY_READY__" not in transcript:
            # A failed disposable build must not retain the temporary shell
            # override.  It remains clearly incomplete, but is safe to retry.
            try:
                set_bootargs(nand, bootargs.encode("ascii"))
            except OSError:
                pass
    if b"__XDJ_FACTORY_READY__" not in transcript:
        raise SystemExit("provisioning failed; inspect runtime/provision*.log")
    ready.write_text(json.dumps({"schema": "xdj-xz-release-runtime/1",
                                 "volumes": [label for _, label, _ in VOLUMES],
                                 "display_bootstrap": args.display_bootstrap == "on"},
                                indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
