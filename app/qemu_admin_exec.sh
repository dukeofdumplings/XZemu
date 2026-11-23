#!/bin/bash
# Execute only QEMU with the privileges requested by BIN/boot-normal.sh --admin.
# The runner, runtime provisioning, Python controls, and log redirection stay
# owned by the interactive user.  A permissive umask lets that user retain
# access to QEMU's local serial socket after sudo starts the emulator.
set -euo pipefail
umask 000
exec "$@"
