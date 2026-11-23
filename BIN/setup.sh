#!/bin/bash
# Opens the local XZemu runtime builder. It never writes physical media.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VENV="$ROOT/.venv"
PYTHON="$VENV/bin/python3"

# The updater extractor needs cryptography. Keep this package
# project-local so launching setup does not depend on whichever Python package
# set happens to be installed globally on this Mac.
if [ ! -x "$PYTHON" ]; then
    python3 -m venv "$VENV"
fi
if ! "$PYTHON" -c 'import cryptography' >/dev/null 2>&1; then
    "$PYTHON" -m pip install --disable-pip-version-check -r "$ROOT/app/requirements-runtime.txt"
fi
exec "$PYTHON" "$ROOT/app/prelauncher.py" "$@"
