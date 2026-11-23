#!/bin/bash
# The one root-level entry point for XZemu on macOS.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$ROOT/BIN/setup.sh" "$@"
