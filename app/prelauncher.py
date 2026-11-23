#!/usr/bin/env python3
"""Local XZemu runtime builder and launcher for macOS.

The original updater supplies verified firmware payloads.  It does *not*
describe a generic USB-stick filesystem, so this tool creates a disposable
virtual NAND runtime instead.  USB media remains an explicit sidecar action.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import queue
import shlex
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from urllib.parse import unquote, urlparse


ROOT = Path(__file__).resolve().parents[1]
PROJECT_PYTHON = ROOT / ".venv/bin/python3"
SETTINGS = Path.home() / "Library/Application Support/XZemu/prelauncher.json"
DEFAULT_KEY_PATH = SETTINGS.parent / "aes256.key"
PARTITIONS = (
    ("Boot", "0x00000000", "3 MiB"),
    ("ENV", "0x00400000", "1 MiB"),
    ("Kernel A", "0x00500000", "10 MiB"),
    ("Rootfs", "0x01900000", "30 MiB"),
    ("Settings", "0x03700000", "10 MiB"),
    ("PDJ", "0x04100000", "8 MiB"),
    ("GUI", "0x06c00000", "40 MiB"),
)
ENV_OFFSET = 0x00400000
ENV_SCOPE = 128 * 1024


def read_settings() -> dict[str, str]:
    try:
        value = json.loads(SETTINGS.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def write_settings(value: dict[str, str]) -> None:
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    temporary = SETTINGS.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(SETTINGS)


def normalize_user_path(value: str) -> Path:
    """Normalize a path pasted from Finder, Terminal, or a shell command.

    Tk entries contain pasted text literally.  In particular, a user may paste
    a quoted shell path (``'/path with spaces/file'``) or Finder's
    ``file:///path`` form.  Neither is a filename as far as ``Path`` is
    concerned, so normalize only those unambiguous wrappers; an ordinary path
    containing spaces is otherwise left intact.
    """
    text = value.strip()
    if text.startswith("file:"):
        parsed = urlparse(text)
        if parsed.scheme == "file" and parsed.netloc in ("", "localhost"):
            text = unquote(parsed.path)
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        text = text[1:-1]
    return Path(text).expanduser()


def normalize_path_field(variable: tk.StringVar) -> Path:
    """Persist a canonical, visible spelling for a user-entered path."""
    path = normalize_user_path(variable.get())
    if variable.get().strip():
        variable.set(str(path))
    return path


def configured_key_path(saved: dict[str, str]) -> str:
    """Return an existing local key path without ever reading its contents."""
    candidates = (
        saved.get("key", ""),
        os.environ.get("XDJXZ_KEY", ""),
        str(DEFAULT_KEY_PATH),
    )
    for candidate in candidates:
        if candidate and normalize_user_path(candidate).is_file():
            return str(normalize_user_path(candidate))
    return ""


def most_recent_runtime() -> Path | None:
    """Offer the latest complete user runtime when Setup is reopened."""
    candidates = []
    for runtime in (ROOT / "runtime").glob("xdj-xz-*"):
        if all((runtime / name).is_file()
               for name in ("nand.bin", "bootstrap.json", "firmware/u-boot.bin.nand", "recovery.dmg")):
            candidates.append(runtime)
    return max(candidates, key=lambda path: path.stat().st_mtime) if candidates else None


class Prelauncher(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("XZemu Setup")
        self.minsize(820, 600)
        self.messages: queue.Queue[tuple[str, str]] = queue.Queue()
        self.busy = False
        saved = read_settings()
        suggested_runtime = ROOT / "runtime" / (
            "xdj-xz-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        saved_runtime_value = saved.get("runtime", "")
        saved_runtime = (normalize_user_path(saved_runtime_value)
                         if saved_runtime_value else None)
        initial_runtime = (saved_runtime if saved_runtime and saved_runtime.is_dir()
                           else most_recent_runtime() or suggested_runtime)
        self.upd = tk.StringVar(value=saved.get("upd", ""))
        self.key = tk.StringVar(value=configured_key_path(saved))
        self.runtime = tk.StringVar(value=str(initial_runtime))
        self.display_bootstrap = tk.BooleanVar(value=True)
        self.show_serial_boot = tk.BooleanVar(value=False)
        self.runtime_state = tk.StringVar()
        self.status = tk.StringVar(value="Choose a runtime to boot, or build one from the updater.")
        self._build()
        self.runtime.trace_add("write", lambda *_: self._refresh_runtime_state())
        self._refresh_runtime_state()
        self.after(50, self._drain)
        self.after(1000, self._poll_runtime_state)

    def _build(self) -> None:
        root = ttk.Frame(self, padding=16)
        root.grid(sticky="nsew")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        root.columnconfigure(1, weight=1)

        ttk.Label(root, text="XZemu runtime setup", font=("TkDefaultFont", 18, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(root, text="Build a recovery-ready runtime, or choose one to boot.",
                  wraplength=780).grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(3, 12))

        self._file_row(root, 2, "XDJXZ.UPD", self.upd, self._choose_upd)
        self._file_row(root, 3, "AES Key", self.key, self._choose_key)
        self._directory_row(root, 4, "Runtime folder", self.runtime)

        ttk.Checkbutton(root, text="Bypass hardware GPU acceleration (currently required)",
                        variable=self.display_bootstrap).grid(
                            row=5, column=0, columnspan=3, sticky="w", pady=(5, 2))
        ttk.Checkbutton(root, text="Show serial boot console",
                        variable=self.show_serial_boot).grid(
                            row=6, column=0, columnspan=3, sticky="w", pady=(2, 2))
        self.create_button = ttk.Button(root, text="Build recovery runtime", command=self._create)
        self.create_button.grid(row=7, column=0, columnspan=3, sticky="ew", pady=(8, 0))
        self.recovery_button = ttk.Button(root, text="Run updater and install firmware",
                                          command=lambda: self._launch("recovery"))
        self.recovery_button.grid(row=8, column=0, columnspan=3, sticky="ew", pady=(6, 0))

        boot_actions = ttk.Frame(root)
        boot_actions.grid(row=9, column=0, columnspan=3, sticky="ew", pady=(6, 0))
        boot_actions.columnconfigure(0, weight=1)
        boot_actions.columnconfigure(1, weight=1)
        self.normal_button = ttk.Button(boot_actions, text="Boot normal",
                                        command=lambda: self._launch("normal"))
        self.normal_button.grid(row=0, column=0, sticky="ew", padx=(0, 3))
        self.admin_button = ttk.Button(boot_actions,
                                       text="Boot admin (required for physical USB)",
                                       command=lambda: self._launch("admin"))
        self.admin_button.grid(row=0, column=1, sticky="ew", padx=(3, 0))
        ttk.Label(root, textvariable=self.runtime_state, foreground="#5bcf72", wraplength=780).grid(
            row=10, column=0, columnspan=3, sticky="w", pady=(8, 2))
        ttk.Label(root, textvariable=self.status, foreground="white", wraplength=780).grid(
            row=11, column=0, columnspan=3, sticky="w", pady=(8, 4))
        self.log = tk.Text(root, height=11, wrap="word", state="disabled")
        self.log.grid(row=12, column=0, columnspan=3, sticky="nsew")
        root.rowconfigure(12, weight=1)

    def _file_row(self, parent, row: int, label: str, value: tk.StringVar, chooser) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=3)
        entry = ttk.Entry(parent, textvariable=value)
        entry.grid(row=row, column=1, sticky="ew", padx=6, pady=3)
        entry.bind("<FocusOut>", lambda _event: normalize_path_field(value))
        entry.bind("<Return>", lambda _event: normalize_path_field(value))
        ttk.Button(parent, text="Choose…", command=chooser).grid(row=row, column=2, sticky="ew", pady=3)

    def _directory_row(self, parent, row: int, label: str, value: tk.StringVar) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=3)
        entry = ttk.Entry(parent, textvariable=value)
        entry.grid(row=row, column=1, sticky="ew", padx=6, pady=3)
        entry.bind("<FocusOut>", lambda _event: normalize_path_field(value))
        entry.bind("<Return>", lambda _event: normalize_path_field(value))
        ttk.Button(parent, text="Choose…", command=lambda: self._choose_directory(value)).grid(
            row=row, column=2, sticky="ew", pady=3)

    def _choose_upd(self) -> None:
        value = filedialog.askopenfilename(title="Select verified XDJXZ.UPD")
        if value:
            self.upd.set(value)

    def _choose_key(self) -> None:
        value = filedialog.askopenfilename(title="Select local XDJ-XZ boot key")
        if value:
            self.key.set(value)

    def _choose_directory(self, variable: tk.StringVar) -> None:
        value = filedialog.askdirectory(title="Select a runtime folder")
        if value:
            self.runtime.set(value)

    def _ready_runtime(self) -> bool:
        runtime = normalize_user_path(self.runtime.get())
        return (runtime / "nand.bin").is_file() and (runtime / "ready.json").is_file()

    def _base_runtime(self) -> bool:
        runtime = normalize_user_path(self.runtime.get())
        return all((runtime / name).is_file()
                   for name in ("nand.bin", "bootstrap.json", "firmware/u-boot.bin.nand"))

    def _recovery_runtime(self) -> bool:
        runtime = normalize_user_path(self.runtime.get())
        return self._base_runtime() and (runtime / "recovery.dmg").is_file()

    @staticmethod
    def _env_bootstrap_present(runtime: Path) -> bool:
        """Recognize old ready manifests by their actual stored U-Boot ENV."""
        try:
            with (runtime / "nand.bin").open("rb") as stream:
                stream.seek(ENV_OFFSET + 4)
                env = stream.read(ENV_SCOPE - 4)
            return b"keep_bootcon rdinit=/bin/sh" in env.split(b"\0\0", 1)[0]
        except OSError:
            return False

    def _refresh_runtime_state(self) -> None:
        runtime = normalize_user_path(self.runtime.get())
        if self._ready_runtime():
            try:
                ready = json.loads((runtime / "ready.json").read_text())
                bootstrap = bool(ready.get("display_bootstrap", False))
            except (OSError, json.JSONDecodeError):
                bootstrap = False
            bootstrap = bootstrap or self._env_bootstrap_present(runtime)
            self.runtime_state.set("Ready (GPU bypass available)" if bootstrap else "Ready")
            self.create_button.state(["!disabled"])
            for button in (self.normal_button, self.admin_button, self.recovery_button):
                button.state(["!disabled"])
        elif self._base_runtime():
            bootstrap = self._env_bootstrap_present(runtime)
            recovery_ready = self._recovery_runtime()
            self.runtime_state.set("Ready (GPU bypass available)" if bootstrap else "Ready")
            self.create_button.state(["!disabled"])
            for button in (self.normal_button, self.admin_button):
                button.state(["!disabled"])
            self.recovery_button.state(["!disabled"] if recovery_ready else ["disabled"])
        else:
            self.runtime_state.set("Select an existing runtime, or choose an empty folder and build one.")
            self.create_button.state(["!disabled"])
            for button in (self.normal_button, self.admin_button, self.recovery_button):
                button.state(["disabled"])

    def _poll_runtime_state(self) -> None:
        """Notice a recovery install completed by the separately launched Terminal."""
        self._refresh_runtime_state()
        self.after(1000, self._poll_runtime_state)

    def _append(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _drain(self) -> None:
        while True:
            try:
                kind, payload = self.messages.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self._append(payload)
                if payload.startswith("[XZemu] "):
                    self.status.set(payload.removeprefix("[XZemu] ").strip())
            elif kind == "done":
                self.busy = False
                self.create_button.state(["!disabled"])
                self.status.set(payload)
                self._refresh_runtime_state()
            elif kind == "error":
                self.busy = False
                self.create_button.state(["!disabled"])
                self.status.set(f"Error: {payload}")
                messagebox.showerror("XZemu setup", payload)
        self.after(50, self._drain)

    def _run(self, command: list[str]) -> None:
        self.messages.put(("log", "$ " + " ".join(shlex.quote(item) for item in command) + "\n"))
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, bufsize=1) as process:
            assert process.stdout is not None
            for line in process.stdout:
                self.messages.put(("log", line))
            if process.wait() != 0:
                raise RuntimeError(f"command failed with status {process.returncode}")

    def _create(self) -> None:
        if self.busy:
            return
        upd, key, runtime = (normalize_path_field(self.upd), normalize_path_field(self.key),
                             normalize_path_field(self.runtime))
        invalid = [label for label, path in (("XDJXZ.UPD", upd), ("AES Key", key))
                   if not path.is_file()]
        if invalid:
            messagebox.showerror(
                "XZemu setup",
                "Could not find a regular file for: " + ", ".join(invalid)
                + "\n\nPaste a full path, use Choose…, or check the filename.")
            return
        rebuild = runtime.exists() and any(runtime.iterdir())
        if rebuild and not messagebox.askyesno(
                "Rebuild recovery runtime",
                "NAND and pre-boot image already exist.\n\n"
                "This will recreate the image from scratch and require the "
                "updater to run again.\n\nContinue?"):
            return
        protected_roots = {Path("/"), ROOT.resolve(), (ROOT / "runtime").resolve(),
                           Path.home().resolve()}
        if rebuild and (runtime.is_symlink() or runtime.resolve() in protected_roots):
            messagebox.showerror("XZemu setup", "Choose a regular runtime folder to rebuild.")
            return
        self.busy = True
        self.create_button.state(["disabled"])
        self.status.set("Creating recovery runtime…")
        display_bootstrap = self.display_bootstrap.get()
        python = str(PROJECT_PYTHON if PROJECT_PYTHON.is_file() else sys.executable)
        def worker() -> None:
            try:
                if rebuild:
                    shutil.rmtree(runtime)
                self._run([python, str(ROOT / "app/prepare_runtime.py"),
                           "--upd", str(upd), "--key", str(key), "--runtime", str(runtime),
                           "--display-bootstrap", "on" if display_bootstrap else "off"])
                profile = {
                    "schema": "xzemu-runtime-launch/1",
                    "nand": "nand.bin",
                    "geometry": {"bytes": 0x10000000, "page_bytes": 2048, "oob_bytes": 64,
                                 "pages_per_eraseblock": 64},
                    "partitions": [{"name": name, "offset": offset, "capacity": capacity}
                                   for name, offset, capacity in PARTITIONS],
                    "display": {"model": "BG23 CPMEM scanout", "enabled_by": "BIN/boot-normal.sh",
                                "env_bootstrap": display_bootstrap},
                }
                (runtime / "launch-profile.json").write_text(
                    json.dumps(profile, indent=2) + "\n")
                write_settings({"upd": str(upd), "key": str(key), "runtime": str(runtime)})
                self.messages.put(("done", "Recovery runtime ready. Use Boot recovery updater."))
            except Exception as error:
                self.messages.put(("error", str(error)))
        threading.Thread(target=worker, daemon=True).start()

    def _launch(self, mode: str) -> None:
        runtime = normalize_path_field(self.runtime)
        if mode == "recovery" and not self._recovery_runtime():
            messagebox.showerror("XZemu setup", "Build or select a recovery-ready runtime first.")
            return
        if mode != "recovery" and not self._base_runtime():
            messagebox.showerror("XZemu setup", "Build or select a recovery-ready runtime first.")
            return
        if mode == "normal":
            launcher, arguments = "./BIN/boot-normal.sh", ""
        elif mode == "admin":
            launcher, arguments = "./BIN/boot-admin.sh", ""
        elif mode == "recovery":
            launcher, arguments = "./BIN/boot-recovery.sh", ""
        else:
            raise ValueError(mode)
        if self.show_serial_boot.get():
            arguments += " --show-serial"
        # Retain a manually selected complete runtime too.  Without this the
        # next Setup launch points at a newly timestamped empty directory.
        write_settings({"upd": self.upd.get(), "key": self.key.get(),
                        "runtime": str(runtime)})
        command = (f"cd {shlex.quote(str(ROOT))} && {launcher} --runtime "
                   f"{shlex.quote(str(runtime))}{arguments}")
        subprocess.run(["osascript", "-e", f'tell application "Terminal" to do script {json.dumps(command)}'],
                       check=True)
        self.status.set("Build Terminal")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, help="Open this runtime in the setup window")
    args = parser.parse_args()
    window = Prelauncher()
    if args.runtime:
        window.runtime.set(str(args.runtime))
    window.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
