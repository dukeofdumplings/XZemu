#!/bin/bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_NUMBER=$(tr -d '[:space:]' < "$ROOT/BUILD_NUMBER")
QEMU_VERSION=11.1.2
ARCHIVE="$ROOT/build/qemu-$QEMU_VERSION.tar.xz"
SOURCE="$ROOT/build/qemu-$QEMU_VERSION"
OUTPUT="$ROOT/build/qemu"

for command in curl tar python3 ninja pkg-config rsync patch; do
    command -v "$command" >/dev/null || {
        printf 'missing required host command: %s\n' "$command" >&2
        exit 1
    }
done
xcode-select -p >/dev/null
pkg-config --exists glib-2.0 pixman-1 || {
    printf '%s\n' 'Install build prerequisites first: brew install glib pixman ninja pkg-config' >&2
    exit 1
}

mkdir -p "$ROOT/build"
if [[ ! -f "$ARCHIVE" ]]; then
    curl --fail --location --retry 3 --output "$ARCHIVE.tmp" \
        "https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz"
    mv "$ARCHIVE.tmp" "$ARCHIVE"
fi

if [[ ! -d "$SOURCE" ]]; then
    tar -xJf "$ARCHIVE" -C "$ROOT/build"
fi

# The release branch carries only the XDJ-XZ deltas.  Reapplying the overlay
# to the pinned upstream source is intentional and makes the build independent
# of an ignored workstation state/ directory.
rsync -a --checksum "$ROOT/qemu-overlay/" "$SOURCE/"

# Preserve the source-level ARM MTTCG correction as a small patch instead of
# carrying the whole upstream target/arm helper in the device overlay.
ARM_MTTCP_PATCH="$ROOT/qemu-patches/0012-arm-sev-wfe-mttcg-wakeup.patch"
if ! grep -q 'upstream ARM MTTCG lost-WFE-wakeup fix' \
        "$SOURCE/target/arm/tcg/op_helper.c"; then
    patch --batch -d "$SOURCE" -p1 < "$ARM_MTTCP_PATCH"
fi

if [[ ! -f "$OUTPUT/build.ninja" ]]; then
    mkdir -p "$OUTPUT"
    (
        cd "$OUTPUT"
        "$SOURCE/configure" \
        --target-list=arm-softmmu \
        --enable-cocoa \
        --audio-drv-list=coreaudio \
        --disable-werror \
        --prefix="$ROOT/build/install"
    )
fi
if ! ninja -C "$OUTPUT" qemu-system-arm; then
    # QEMU's macOS post-link helper occasionally loses its temporary output
    # while the unsigned target itself was linked successfully.  Retry just
    # that local packaging step; do not mask a failed compile or link.
    if [[ ! -x "$OUTPUT/qemu-system-arm-unsigned" ]]; then
        exit 1
    fi
    "$SOURCE/scripts/entitlement.sh" \
        "$OUTPUT/qemu-system-arm" \
        "$OUTPUT/qemu-system-arm-unsigned" \
        "$SOURCE/pc-bios/qemu.rsrc" \
        ""
fi
[[ -x "$OUTPUT/qemu-system-arm" ]]

python3 - "$ROOT/build/release-build.json" "$BUILD_NUMBER" "$QEMU_VERSION" <<'PY'
import json, pathlib, subprocess, sys
out, build, qemu = map(str, sys.argv[1:])
pathlib.Path(out).write_text(json.dumps({
    "build_number": int(build),
    "qemu_version": qemu,
    "qemu_binary": "build/qemu/qemu-system-arm",
}, indent=2) + "\n", encoding="utf-8")
PY
printf 'XDJ-XZ macOS build %s: %s\n' "$BUILD_NUMBER" "$OUTPUT/qemu-system-arm"
