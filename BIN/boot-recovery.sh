#!/bin/bash
# Explicit original-Pioneer recovery updater boot.  Normal boots never attach
# updater USB media; use this runner only when deliberately entering recovery.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$ROOT/BIN/boot-normal.sh" --recovery "$@"
