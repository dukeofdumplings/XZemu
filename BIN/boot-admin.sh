#!/bin/bash
# Explicit physical-USB launch profile. Runtime preparation and the Python
# controls stay as the desktop user; boot-normal.sh elevates only QEMU.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$ROOT/BIN/boot-normal.sh" --admin "$@"
