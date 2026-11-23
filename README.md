# XZemu

Boot real XDJ-XZ firmware in QEMU. XZemu is a set of QEMU i.MX6 changes plus a
small macOS launcher/controls app. It aims to keep the original Pioneer Linux
and RBP application in charge, while emulating enough of the board around them
to make safe firmware experimentation a reality.

## Why this exists

XZemu is for people who want to test, build, and modify things for the XDJ-XZ
in an environment that behaves much more like the real hardware. It is not
limited to reworking the Pioneer application: it can be used as a base for
rekordbox app replacement, or custom OS builds for PDJ.

It keeps the original Pioneer Linux and RBP application talking to SoC-facing
contracts: NAND through GPMI/APBH/BCH, sub-CPU reports through ECSPI, display
through IPU, and the original USB, network, audio, and multi-core paths.

That means less hardware abstraction and fewer “just make it work" shortcuts.
It is useful when the question is whether original hardware will initialize
against real XZ, or when a custom payload needs to be tested with less risk
to a real XDJ-XZ on the desk.

## What it is

- A QEMU expansion for the XDJ-XZ i.MX6 platform.
- A way to boot the original Pioneer Linux and RBP environment with user-supplied
  firmware.
- A hardware-oriented starting point for offline testing or for building custom
  Pioneer-compatible software from scratch.
- A foundation that may be useful for related Qualcomm/i.MX Pioneer systems,
  including the XDJ-RX, RX2, and RX3. Those are not supported targets yet.

Current work includes the original Linux boot path, virtual NAND, IPU-backed
display output, USB and network topology, selected physical controls, and the
ARM WFE/SEV and vCPU-affinity work needed to make the four-core guest usable.

## What it is not

- Not a firmware distribution. No `XDJXZ.UPD`, AES key, firmware image, or
  captured USB media is included. Supply only media you are entitled to use.
- A new runtime starts with unprovisioned NAND regions erased. An optional
  settings capture can be supplied privately to the runtime builder, but none
  is included here.
- Not a complete circuit-level reproduction of every XDJ-XZ peripheral.
- Not affiliated with, endorsed by, or sponsored by AlphaTheta or Pioneer DJ.

## Build on macOS

Requirements:

```bash
xcode-select --install
brew install glib pixman ninja pkg-config
```

Build the QEMU binary:

```bash
./BIN/build-macos.sh
```

This downloads pinned upstream QEMU 11.1.2, applies the tracked XZemu overlay
and ARM timing patch, and produces:

```text
build/qemu/qemu-system-arm
```

Open the Python runtime-builder UI by double-clicking `XZemu Setup.command` in
Finder, or from Terminal:

```bash
open "XZemu Setup.command"
```

`BIN/setup.sh` is the lower-level helper used by that UI. The setup UI uses a
project-local Python virtual environment and may install the Python
`cryptography` package. Tkinter must be available in the selected Python
installation.

## Known issues

- USB storage still needs wider stability testing. Use a known-good, fast disk
  image where possible. RBP is unusually sensitive to USB state, and direct
  physical USB attachment in admin mode can freeze an emulated run.
- The original Galcore GPU path is not complete. Use the launcher’s GPU-bypass
  option, which creates the currently required IPU-display bootstrap.
- Touchscreen emulation is not implemented; it is not needed for most current
  XDJ-XZ workflows.
- The original updater UI can display a failure even when its relevant writes
  have completed. Close and boot normally afterward upon "MAIN update failue".
- A newly built runtime **may fail on its first boot**: its settings partition
  and other unprovisioned NAND regions start erased. This is separate from the
  occasional one-off hardware-emulation glitches below.
- Pioneer app can occasionally reach a black screen during boot.
- Audio can crackle because the original audio stack has tight real-time timing
  requirements.
- Hardware mixer FX and channel-volume controls are not implemented. They were
  outside the scope of the initial release work.
- Not every deck key is mapped. A few addresses still disagree between RBP’s
  internal interpretation and the synthetic SPI reports.
- Do not put emulator too deep in the folders. There is a launch string depth
  restrictions that may cause problems. Somewhere within documents or desktop
  is totally fine.

## Platform status

The build and launcher are currently macOS-focused. They have been exercised
on Intel Macs and are expected to work on Apple Silicon after a local build.
Windows support will require separate audio and launcher/sidecar work.

## Upstream sources

- [QEMU 11.1.2 source tarball](https://download.qemu.org/qemu-11.1.2.tar.xz)
  and [QEMU downloads](https://www.qemu.org/download/)
- [Pioneer DJ GPL/LGPL source-code distribution](https://www.pioneerdj.com/en/support/open-source-code-distribution/gnu-open-source-license/)

## Licensing

This source release is licensed under **GNU GPL version 2**. This is the
conservative common baseline: Pioneer Linux 3.0.101 explicitly identifies
itself as GPLv2-only, and the QEMU files used by the overlay are GPLv2 or later.

XZemu does not vendor upstream QEMU itself; the build script downloads its
pinned upstream release. Files in `qemu-overlay/` that derive from QEMU retain
their applicable QEMU copyright notices. Pioneer source material and firmware
are not included and remain subject to their respective licenses. Review the
applicable licenses before redistribution.

