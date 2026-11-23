#!/bin/bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# Portable launchers never assume a host USB image. Attach one explicitly
# through the controls sidecar or --usb-image when an image is wanted.
DEFAULT_USB_IMAGE=''
# Setup always supplies this. Command-line callers must be equally explicit so
# a release checkout never points at a developer's historical runtime.
RUNTIME=${XDJXZ_RUNTIME:-}
QEMU="$ROOT/build/qemu/qemu-system-arm"
UPD=${XDJXZ_UPD:-}
KEY=${XDJXZ_KEY:-}
# This is a sidecar chooser default only.  USB media is never attached on
# boot: the user selects USB1 or USB2 and presses Attach image explicitly.
# XDJXZ_USB_A1 remains a compatibility alias for older local launch profiles.
USB_IMAGE=${XDJXZ_USB_IMAGE:-${XDJXZ_USB_A1:-"$DEFAULT_USB_IMAGE"}}
QEMU_ADMIN=${XDJXZ_QEMU_ADMIN:-off}
# Recovery media is deliberately CLI-only. A normal run must not inherit a
# stale shell variable and silently attach the updater as USB storage.
RECOVERY_MEDIA=''
RECOVERY_REQUEST=off
SHOW_SERIAL=off

choose_file() {
    osascript -e 'POSIX path of (choose file with prompt "Select XDJXZ.UPD")'
}

while (($#)); do
    case "$1" in
        --upd) UPD=$2; shift 2 ;;
        --key) KEY=$2; shift 2 ;;
        --runtime) RUNTIME=$2; shift 2 ;;
        --usb-image|--usb-a1) USB_IMAGE=$2; shift 2 ;;
        --recovery) RECOVERY_REQUEST=on; shift ;;
        --recovery-media) RECOVERY_MEDIA=$2; shift 2 ;;
        --admin) QEMU_ADMIN=on; shift ;;
        --show-serial) SHOW_SERIAL=on; shift ;;
        *) printf 'usage: %s [--upd XDJXZ.UPD --key aes256.key --runtime DIR --usb-image image.raw [--recovery | --recovery-media image.dmg] --admin --show-serial]\n' "$0" >&2; exit 2 ;;
    esac
done

[[ -n "$RUNTIME" ]] || {
    printf '%s\n' 'select a runtime in XZemu Setup or pass --runtime DIR' >&2
    exit 2
}

[[ -x "$QEMU" ]] || { printf 'build QEMU first: %s/BIN/build-macos.sh\n' "$ROOT" >&2; exit 1; }
case "$QEMU_ADMIN" in
    on|off) ;;
    *) printf 'XDJXZ_QEMU_ADMIN must be on or off, got: %s\n' "$QEMU_ADMIN" >&2; exit 2 ;;
esac
if [[ "$QEMU_ADMIN" == on ]]; then
    command -v sudo >/dev/null || { printf '%s\n' 'sudo is required for --admin' >&2; exit 1; }
    # Authenticate before starting QEMU so a bad password cannot leave a
    # half-created live run. Only the emulator below runs as root.
    sudo -v
fi
# The local control sidecar and its QMP endpoint belong to one live XZemu VM.
# Do not mistake another instance's endpoint for this launch succeeding.
if python3 - <<'PY'
import socket
channel = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
channel.settimeout(0.2)
try:
    channel.connect(("127.0.0.1", 12596))
except OSError:
    raise SystemExit(1)
raise SystemExit(0)
PY
then
    printf '%s\n' 'An XZemu instance is already running. Stop it before starting another one.' >&2
    exit 1
fi
if [[ ! -f "$RUNTIME/nand.bin" ]]; then
    [[ -n "$UPD" ]] || UPD=$(choose_file)
    [[ -n "$KEY" ]] || { printf 'set XDJXZ_KEY or pass --key for the published boot key\n' >&2; exit 2; }
    PREPARE_PY="$ROOT/.venv/bin/python3"
    [[ -x "$PREPARE_PY" ]] || PREPARE_PY=python3
    "$PREPARE_PY" "$ROOT/app/prepare_runtime.py" --upd "$UPD" --key "$KEY" --runtime "$RUNTIME" \
        --display-bootstrap on
fi

[[ -f "$RUNTIME/bootstrap.json" ]] || { printf '%s\n' 'runtime is missing its bootstrap manifest; rebuild it in Setup' >&2; exit 1; }
if [[ "$RECOVERY_REQUEST" == on ]]; then
    RECOVERY_MEDIA="$RUNTIME/recovery.dmg"
fi
UBOOT="$RUNTIME/firmware/u-boot.bin.nand"
[[ -f "$UBOOT" ]] || { printf 'runtime lacks extracted U-Boot; recreate it with a verified UPD\n' >&2; exit 1; }
RECOVERY_ARGS=()
machine='sabrelite,xz-m16-erp=panel-first-reports-tempo-center,xz-mixer=synthetic-menu,xz-jog=synthetic-play-cue,xz-tusb4020bi=on,xz-rtl8309m=on'
if [[ -n "$RECOVERY_MEDIA" ]]; then
    [[ -f "$RECOVERY_MEDIA" ]] || { printf 'recovery medium is not a regular file: %s\n' "$RECOVERY_MEDIA" >&2; exit 2; }
    # Recovery is repeatable: a preceding recovery attempt deliberately
    # returns this disposable runtime to normal boot, so arm update mode again
    # immediately before presenting recovery media.
    python3 "$ROOT/app/finalize_runtime.py" --runtime "$RUNTIME" --set-boot-type update
    # Present the recovery USB materialized from this runtime's original UPD.
    RECOVERY_ARGS=(
        -drive "file=$RECOVERY_MEDIA,format=raw,if=none,readonly=on,id=xz-recovery-media"
        -device 'usb-storage,id=xz-recovery-usb,bus=usb-bus.1,port=1.2,drive=xz-recovery-media'
    )
    # The original updater asks the ERP controller to enter update mode over
    # ECSPI4 CS0.  `accept` is the existing request-validated responder for
    # that updater-only exchange; normal RBP boots retain their panel report.
    machine='sabrelite,xz-m16-erp=accept,xz-mixer=synthetic-menu,xz-jog=synthetic-play-cue,xz-tusb4020bi=on,xz-rtl8309m=on'
fi
mkdir -p "$RUNTIME/live"
tag="$(date -u +%Y%m%dT%H%M%SZ).$$"
qmp="/tmp/xdj-xz-$tag.qmp.sock"
serial="/tmp/xdj-xz-$tag.serial.sock"
log="$RUNTIME/live/$tag.qemu.log"
serial_log="$RUNTIME/live/$tag.serial.log"
sidecar_pid=''
qemu_pid=''

cleanup() {
    [[ -n "$sidecar_pid" ]] && kill "$sidecar_pid" 2>/dev/null || true
    [[ -n "$qemu_pid" ]] && kill "$qemu_pid" 2>/dev/null || true
    [[ -n "$qemu_pid" ]] && wait "$qemu_pid" 2>/dev/null || true
    if [[ -n "$RECOVERY_MEDIA" && -f "$serial_log" ]]; then
        python3 "$ROOT/app/finalize_runtime.py" --runtime "$RUNTIME" \
            --serial-log "$serial_log" --normal-after-recovery || true
    fi
    rm -f "$qmp" "$serial"
}
trap cleanup EXIT INT TERM

qemu_args=(
    -M "$machine"
    -smp 4 -m 1024 -monitor none
    -serial "unix:$serial,server=on,wait=off" -serial null -serial null -serial null -serial null
    -device "loader,file=$UBOOT,addr=0x17800000,cpu-num=0,force-raw=on"
    -global "imx6-gpmi-reset.virtual-nand=$RUNTIME/nand.bin"
    -global fsl-imx6.nand-boot-mode=on
    -global imx6-gpmi-reset.virtual-nand-ram-cache=on
    -global fsl-imx6.openvg-reset-diagnostic-enable=on
    -global fsl-imx6.gc2000-diagnostic-enable=on
    -global fsl-imx6.gc2000-diagnostic-reference-identity=on
    -global fsl-imx6.gc320-diagnostic-enable=on
    -global fsl-imx6.gc320-diagnostic-gc320-model=on
    -global fsl-imx6.ipu1-trace-diagnostic-enable=on
    -global fsl-imx6.ipu1-bg23-eof-diagnostic-enable=on
    -global fsl-imx6.ipu1-bg23-cpmem-scanout-diagnostic-enable=on
    -global fsl-imx6.sdma-cyclic-diagnostic-enable=on
    -global fsl-imx6.sdma-cyclic-audio-enable=on
    -global fsl-imx6.sdma-cyclic-audio-catchup-enable=on
    -global 'imx-rtl8309m.external-link-mask=7'
    -global 'imx-rtl8309m.dataplane-enable=on'
    -device 'xdj-tusb4020bi,id=xz-ti-hub,bus=usb-bus.1,port=1,ports=2'
)
if [[ -n "$RECOVERY_MEDIA" ]]; then
    qemu_args+=("${RECOVERY_ARGS[@]}")
fi
qemu_args+=(
    -netdev user,id=xz-link1 -net nic,model=imx-rtl8309m,netdev=xz-link1
    -netdev socket,id=xz-link2,udp=127.0.0.1:12598,localaddr=127.0.0.1:12608 -net nic,model=imx-rtl8309m,netdev=xz-link2
    -netdev socket,id=xz-link3,udp=127.0.0.1:12599,localaddr=127.0.0.1:12609 -net nic,model=imx-rtl8309m,netdev=xz-link3
    -audio driver=coreaudio,out.frequency=44100,out.buffer-length=23220,out.buffer-count=16
    -global imx6-ssi.host-audio-enable=on
    -global imx6-ssi.host-audio-s24le=on
    -global imx6-ssi.host-audio-callback-only=on
    -global imx6-ssi.host-audio-lazy-activate=on
    -display cocoa
    -qmp "unix:$qmp,server=on,wait=off"
    -qmp 'tcp:127.0.0.1:12596,server=on,wait=off'
)
qemu_command=("$QEMU")
if [[ "$QEMU_ADMIN" == on ]]; then
    qemu_command=(sudo "$ROOT/app/qemu_admin_exec.sh" "$QEMU")
fi
"${qemu_command[@]}" "${qemu_args[@]}" >"$log" 2>&1 &
qemu_pid=$!

qmp_ready=0
for _ in $(seq 1 100); do
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        cat "$log" >&2
        exit 1
    fi
    if python3 - <<'PY'
import socket
channel = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
channel.settimeout(0.1)
try:
    channel.connect(("127.0.0.1", 12596))
except OSError:
    raise SystemExit(1)
finally:
    channel.close()
PY
    then
        qmp_ready=1
        break
    fi
    sleep 0.1
done
if [[ "$qmp_ready" != 1 ]]; then
    printf '%s\n' 'QEMU did not make its QMP endpoint available' >&2
    exit 1
fi

sidecar_args=(--qmp-port 12596 --usb-hotplug on --erp-tempo on
              --qemu-admin "$QEMU_ADMIN" --usb-default "$USB_IMAGE")
if [[ -n "$RECOVERY_MEDIA" ]]; then
    sidecar_args+=(--usb-a1-pre-attached)
fi
python3 "$ROOT/app/control_sidecar.py" "${sidecar_args[@]}" &
sidecar_pid=$!
serial_args=(--serial "$serial" --log "$serial_log")
if [[ "$SHOW_SERIAL" == on ]]; then
    serial_args+=(--show)
fi
python3 "$ROOT/app/serial_relay.py" "${serial_args[@]}"
