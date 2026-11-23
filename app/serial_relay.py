#!/usr/bin/env python3
"""Relay Pioneer UART and inject the validated process-local DirectFB mode."""
from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path

DFBARGS = "system=fbdev,fbdev=/dev/fb0,no-hardware,disable-module=gal,vsync-none"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True, type=Path)
    parser.add_argument("--log", type=Path,
                        help="append the unmodified guest UART stream to this file")
    parser.add_argument("--show", action="store_true",
                        help="mirror guest UART output to this process's console")
    args = parser.parse_args()
    deadline = time.monotonic() + 180
    channel = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            channel.connect(str(args.serial))
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit("guest serial endpoint did not become available")
            time.sleep(0.1)
    injected = False
    seen = bytearray()
    log = args.log.open("ab", buffering=0) if args.log else None
    try:
        with channel:
            while True:
                payload = channel.recv(65536)
                if not payload:
                    return 0
                if args.show:
                    sys.stdout.buffer.write(payload)
                    sys.stdout.buffer.flush()
                if log:
                    log.write(payload)
                seen.extend(payload)
                if injected or b"/ # " not in seen:
                    continue
                command = f"export DFBARGS='{DFBARGS}'; exec /init\r".encode("ascii")
                for byte in command:
                    channel.sendall(bytes((byte,)))
                    time.sleep(0.002)
                injected = True
    finally:
        if log:
            log.close()


if __name__ == "__main__":
    raise SystemExit(main())
