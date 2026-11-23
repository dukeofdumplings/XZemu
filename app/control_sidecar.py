#!/usr/bin/env python3
"""XZemu local controls for the XDJ-XZ QMP mixer, jog, ERP and USB paths."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import plistlib
import queue
import socket
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

BUTTONS = {
    "MENU / UTILITY": "menu-high", "rekordbox": "source-rekordbox-high",
    "SOURCE USB 1": "source-usb1-high", "SOURCE USB 2": "source-usb2-high",
    "LINK": "source-link-high", "BROWSE": "browse-high", "TAG LIST": "tag-list-high",
    "INFO": "info-high", "BACK": "back-high",
    "LOAD 1": "load-deck1-high", "LOAD 2": "load-deck2-high",
    "SHORTCUT": "shortcut-high", "TAG TRACK": "tag-track-high",
    "TRACK FILTER": "track-filter-high", "TIME MODE": "time-mode-high",
    "DECK QUANTIZE": "deck-quantize-high",
}
MIXER_PATH = "/machine/peripheral/xz-mixer"
ERP_PATH = "/machine/peripheral/xz-erp"
JOG_PATH = {1: "/machine/peripheral/xz-jog-left", 2: "/machine/peripheral/xz-jog-right"}
USB_SLOT = {
    # USB1 is pre-attached by the normal launcher at root port 1.2.  The
    # ChipIdea controller exposes the sibling hotplug address as 1.1, not a
    # nonexistent root port 1.3.  Use the exact addresses QEMU reports.
    1: ("xz-sidecar-usb1", "xz-sidecar-usb1-media", "1.2"),
    2: ("xz-sidecar-usb2", "xz-sidecar-usb2-media", "1.1"),
}
SETTINGS_PATH = Path.home() / "Library/Application Support/XZemu/controls.json"
DEFAULT_WINDOW_WIDTH = 920
DEFAULT_WINDOW_HEIGHT = 1000


def load_settings() -> dict[str, object]:
    """Read only harmless host-side sidecar preferences."""
    try:
        with SETTINGS_PATH.open() as stream:
            value = json.load(stream)
        return value if isinstance(value, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_settings(value: dict[str, object]) -> None:
    SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary = SETTINGS_PATH.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(SETTINGS_PATH)


def _usb_inventory() -> list[dict[str, object]]:
    """List physical USB mass-storage devices, mounted or otherwise.

    macOS removes a volume from /Volumes as soon as ``diskutil unmountDisk``
    succeeds, but the USB disk and its libusb identity remain available.  The
    sidecar must retain that identity so an already-unmounted device can still
    be offered for QEMU usb-host attachment.
    """
    try:
        inventory = json.loads(subprocess.check_output(
            ["system_profiler", "-json", "SPUSBDataType"], text=True,
            stderr=subprocess.DEVNULL))
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError):
        return []

    def descendants(value):
        if isinstance(value, dict):
            yield value
            for child in value.values():
                yield from descendants(child)
        elif isinstance(value, list):
            for child in value:
                yield from descendants(child)

    found: list[dict[str, object]] = []
    for device in descendants(inventory.get("SPUSBDataType", [])):
        for media in device.get("Media", []):
            disk = media.get("bsd_name")
            if not isinstance(disk, str) or not disk.startswith("disk"):
                continue
            try:
                info = plistlib.loads(subprocess.check_output(
                    ["diskutil", "info", "-plist", f"/dev/{disk}"],
                    stderr=subprocess.DEVNULL))
            except (OSError, plistlib.InvalidFileException,
                    subprocess.CalledProcessError):
                continue
            if (not info.get("RemovableMediaOrExternalDevice") or
                    info.get("BusProtocol") != "USB"):
                continue
            vendor = str(device.get("vendor_id", "")).split()[0]
            product = str(device.get("product_id", ""))
            try:
                mounted = next((entry.get("mount_point") for entry in media.get("volumes", [])
                                if isinstance(entry, dict) and entry.get("mount_point")), "")
                found.append({
                    "volume": str(mounted),
                    "mounted": bool(mounted),
                    "disk": disk,
                    "name": device.get("_name", disk),
                    "vendorid": int(vendor, 16),
                    "productid": int(product, 16),
                    "serial": device.get("serial_num", ""),
                })
            except ValueError:
                continue
    return found


def physical_usb_volume(volume: str = "/Volumes/A") -> dict[str, object] | None:
    """Return the physical USB device owning an existing mounted volume."""
    return next((device for device in _usb_inventory()
                 if device["volume"] == volume), None)


def physical_usb_volumes() -> list[dict[str, object]]:
    """Discover removable USB storage, including deliberately unmounted disks."""
    return _usb_inventory()


class Qmp:
    """One short, serialized QMP connection per operation."""
    def __init__(self, port: int):
        self.port = port
        self.lock = threading.Lock()

    def _read(self, stream, expected: int | None = None):
        while True:
            line = stream.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            message = json.loads(line)
            if expected is None or message.get("id") == expected:
                return message

    def call(self, name: str, arguments: dict | None = None):
        with self.lock, socket.create_connection(("127.0.0.1", self.port), timeout=4) as channel:
            stream = channel.makefile("rwb", buffering=0)
            self._read(stream)
            stream.write(b'{"execute":"qmp_capabilities","id":1}\n')
            self._result(self._read(stream, 1))
            request = {"execute": name, "id": 2}
            if arguments is not None:
                request["arguments"] = arguments
            stream.write((json.dumps(request) + "\n").encode())
            return self._result(self._read(stream, 2))

    @staticmethod
    def _result(message):
        if "error" in message:
            error = message["error"]
            raise RuntimeError(f"{error.get('class', 'QMP')}: {error.get('desc', 'request failed')}")
        return message.get("return")


class Controls(tk.Tk):
    def __init__(self, qmp_port: int, usb_hotplug: bool, erp_tempo: bool,
                 usb_default: str | None, usb_a1_pre_attached: bool,
                 qemu_admin: bool):
        super().__init__()
        self.title("XZemu Controls")
        self.minsize(640, 480)
        self.resizable(True, True)
        self.qmp, self.usb_hotplug, self.erp_tempo = Qmp(qmp_port), usb_hotplug, erp_tempo
        self.pending = queue.Queue()
        self.usb_slots: dict[int, tuple[str, str]] = {}
        self.usb_a1_pre_attached = usb_a1_pre_attached
        self.qemu_admin = qemu_admin
        self.physical_usb_candidates: dict[str, dict[str, object]] = {}
        self.physical_usb_picker: dict[int, ttk.Combobox] = {}
        self.settings = load_settings()
        self.tempo_after: dict[int, str] = {}
        self._window_geometry_after: str | None = None
        self.status = tk.StringVar(value=f"Connecting to QMP on 127.0.0.1:{qmp_port}…")
        saved_paths = self.settings.get("usb_images", {})
        saved_paths = saved_paths if isinstance(saved_paths, dict) else {}
        self.usb_path = {
            1: tk.StringVar(value=str(saved_paths.get("1") or usb_default or "")),
            2: tk.StringVar(value=str(saved_paths.get("2") or "")),
        }
        self.usb_read_only = {1: tk.BooleanVar(value=False), 2: tk.BooleanVar(value=False)}
        self.physical_usb_choice = {1: tk.StringVar(), 2: tk.StringVar()}
        self.usb_slot_text = {1: tk.StringVar(value="Checking QEMU…"),
                              2: tk.StringVar(value="Empty")}
        self.physical_usb_text = tk.StringVar(value="Physical USB: checking…")
        self.tempo = {1: tk.DoubleVar(value=0), 2: tk.DoubleVar(value=0)}
        self._build()
        self._restore_window_geometry()
        self.bind("<Configure>", self._queue_window_geometry, add="+")
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(50, self._drain)
        self.work(self._initial_state, "Connected to local QMP")
        self._refresh_physical_usb()

    def work(self, function, success: str | None = None):
        def run():
            try:
                value = function()
                self.pending.put((True, success, value))
            except Exception as error:  # surface QMP errors in the UI
                self.pending.put((False, str(error), None))
        threading.Thread(target=run, daemon=True).start()

    def _drain(self):
        while True:
            try:
                okay, message, value = self.pending.get_nowait()
            except queue.Empty:
                break
            if okay:
                self.status.set(message or "Done")
                if isinstance(value, dict) and "rotary" in value:
                    self.rotary_count.configure(text=f"count: {value['rotary']}")
                if isinstance(value, dict) and value.get("usb1_static"):
                    device = str(value["usb1_static"])
                    self.usb_slots[1] = ("static", device)
                    self.usb_slot_text[1].set(
                        "Recovery updater attached" if device == "xz-recovery-usb"
                        else "Attached by launcher")
                if isinstance(value, dict) and value.get("usb2_host"):
                    self.usb_slots[2] = ("host", "xz-sidecar-usb2-host")
                    self.usb_slot_text[2].set("Physical USB attached")
            else:
                self.status.set(f"Error: {message}")
        self.after(50, self._drain)

    def _initial_state(self):
        count = self.qmp.call("qom-get", {"path": MIXER_PATH, "property": "rotary-count"})
        usb_topology = self.qmp.call("human-monitor-command", {"command-line": "info usb"})
        usb1_static = ("xz-recovery-usb" if "ID: xz-recovery-usb" in usb_topology
                       else "xz-usba1-bot" if "ID: xz-usba1-bot" in usb_topology else None)
        return {"rotary": count,
                "usb1_static": usb1_static,
                "usb2_host": "ID: xz-sidecar-usb2-host" in usb_topology}

    def _build(self):
        outer = ttk.Frame(self)
        outer.grid(sticky="nsew")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

        canvas = tk.Canvas(outer, highlightthickness=0, borderwidth=0)
        vertical = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        horizontal = ttk.Scrollbar(outer, orient="horizontal", command=canvas.xview)
        canvas.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        canvas.grid(row=0, column=0, sticky="nsew")
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal.grid(row=1, column=0, sticky="ew")
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(0, weight=1)

        root = ttk.Frame(canvas, padding=14)
        content = canvas.create_window((0, 0), window=root, anchor="nw")
        self._canvas = canvas
        self._canvas_root = root
        self._canvas_content = content
        root.bind("<Configure>", self._sync_scroll_region)
        canvas.bind("<Configure>", self._sync_scroll_region)
        self.bind_all("<MouseWheel>", self._scroll_controls, add="+")
        self.bind_all("<Button-4>", lambda _event: canvas.yview_scroll(-1, "units"), add="+")
        self.bind_all("<Button-5>", lambda _event: canvas.yview_scroll(1, "units"), add="+")

        root.columnconfigure(0, weight=1)
        ttk.Label(root, text="XZemu controls", font=("TkDefaultFont", 18, "bold")).grid(row=0, sticky="w")
        self._button_group(root, "Browse / source", BUTTONS.items(), 1)
        self._rotary_group(root, 2)
        self._jog_group(root, 3)
        if self.erp_tempo:
            self._tempo_group(root, 4)
            self._deck_performance_group(root, 5)
        self._usb_group(root, 6)
        ttk.Label(root, textvariable=self.status, foreground="#9ad", wraplength=900).grid(row=7, sticky="w", pady=(10, 0))

    def _sync_scroll_region(self, _event=None):
        """Keep the controls usable on both compact and wide displays."""
        required = self._canvas_root.winfo_reqwidth()
        self._canvas.itemconfigure(self._canvas_content,
                                   width=max(required, self._canvas.winfo_width()))
        self._canvas.configure(scrollregion=self._canvas.bbox("all"))

    def _scroll_controls(self, event):
        if event.widget.winfo_toplevel() is not self or not event.delta:
            return
        self._canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

    def _restore_window_geometry(self):
        """Use a 1080p-friendly default, then honor the shared UI preference."""
        saved = self.settings.get("window_geometry", {})
        saved = saved if isinstance(saved, dict) else {}
        screen_width = self.winfo_screenwidth()
        screen_height = self.winfo_screenheight()
        default_height = min(DEFAULT_WINDOW_HEIGHT, max(480, screen_height - 80))
        try:
            width = int(saved.get("width", DEFAULT_WINDOW_WIDTH))
            height = int(saved.get("height", default_height))
        except (TypeError, ValueError):
            width, height = DEFAULT_WINDOW_WIDTH, default_height
        width = min(max(width, 640), max(640, screen_width - 32))
        height = min(max(height, 480), max(480, screen_height - 80))
        self.geometry(f"{width}x{height}")
        self.after_idle(self._save_window_geometry)

    def _queue_window_geometry(self, event):
        if event.widget is not self:
            return
        if self._window_geometry_after is not None:
            self.after_cancel(self._window_geometry_after)
        self._window_geometry_after = self.after(300, self._save_window_geometry)

    def _save_window_geometry(self):
        self._window_geometry_after = None
        if self.state() == "zoomed":
            return
        self.settings["window_geometry"] = {
            "width": self.winfo_width(),
            "height": self.winfo_height(),
        }
        try:
            save_settings(self.settings)
        except OSError:
            pass

    def _close(self):
        self._save_window_geometry()
        self.destroy()

    def _button_group(self, parent, title, entries, row):
        frame = ttk.LabelFrame(parent, text=title, padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=5)
        for index, (label, prop) in enumerate(entries):
            button = ttk.Button(frame, text=label)
            button.grid(row=index // 5, column=index % 5, padx=3, pady=3, sticky="ew")
            button.bind("<ButtonPress-1>", lambda _e, p=prop, l=label: self._level(MIXER_PATH, p, True, l))
            button.bind("<ButtonRelease-1>", lambda _e, p=prop, l=label: self._level(MIXER_PATH, p, False, l))
        for column in range(5): frame.columnconfigure(column, weight=1)

    def _jog_group(self, parent, row: int):
        frame = ttk.LabelFrame(parent, text="Deck transport", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=5)
        for index, (deck, control) in enumerate(((1, "play"), (1, "cue"), (2, "play"), (2, "cue"))):
            label = f"DECK {deck} {control.upper()}"
            button = ttk.Button(frame, text=label)
            button.grid(row=0, column=index, padx=3, pady=3, sticky="ew")
            path, prop = JOG_PATH[deck], f"{control}-high"
            button.bind("<ButtonPress-1>", lambda _e, p=path, x=prop, l=label: self._level(p, x, True, l))
            button.bind("<ButtonRelease-1>", lambda _e, p=path, x=prop, l=label: self._level(p, x, False, l))
            frame.columnconfigure(index, weight=1)

    def _tempo_group(self, parent, row: int):
        frame = ttk.LabelFrame(parent, text="Tempo fixture", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=5)
        for deck in (1, 2):
            column = ttk.Frame(frame); column.grid(row=0, column=deck - 1, sticky="nsew", padx=6)
            ttk.Label(column, text=f"Deck {deck}").grid(sticky="w")
            scale = ttk.Scale(column, from_=-100, to=100, variable=self.tempo[deck],
                              command=lambda _v, d=deck: self._queue_tempo(d))
            # The scale shares the exact three-column width of RANGE, M/T,
            # and Set to 0.  Do not give it a fixed pixel length: that made
            # Deck 1 expand the whole window on narrower displays.
            scale.grid(row=1, column=0, columnspan=3, sticky="ew")
            for index, (control, label) in enumerate((
                    ("range", "RANGE"), ("master", "M/T"), ("reset", "Set to 0"))):
                button = ttk.Button(column, text=label)
                button.grid(row=2, column=index, padx=2, pady=3, sticky="ew")
                prop = f"tempo-deck{deck}-{control}-high"
                if control == "reset":
                    button.bind("<ButtonPress-1>",
                                lambda _e, p=prop, d=deck: self._tempo_reset_press(d, p))
                else:
                    button.bind("<ButtonPress-1>", lambda _e, p=prop, d=deck, c=control: self._level(ERP_PATH, p, True, f"Deck {d} {c}"))
                button.bind("<ButtonRelease-1>", lambda _e, p=prop, d=deck, c=control: self._level(ERP_PATH, p, False, f"Deck {d} {c}"))
                column.columnconfigure(index, weight=1)
            frame.columnconfigure(deck - 1, weight=1)

    def _deck_performance_group(self, parent, row: int):
        frame = ttk.LabelFrame(parent, text="Deck performance", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=5)
        for deck in (1, 2):
            group = ttk.LabelFrame(frame, text=f"Deck {deck}", padding=6)
            group.grid(row=0, column=deck - 1, sticky="nsew", padx=5)
            for column in range(4):
                group.columnconfigure(column, weight=1, uniform="deck-control")
            # SYNC and MASTER use the source-mapped ERP byte-10 controls.
            # They are distinct from the tempo-master fixture above.
            self._orange_level_button(group, "SYNC", deck, "sync", 0, 0, 2)
            self._orange_level_button(group, "MASTER", deck, "master", 0, 2, 2)
            self._orange_level_button(group, "REVERSE", deck, "reverse", 1, 0, 2)
            self._orange_level_button(group, "EXIT LOOP", deck, "reloop-exit", 1, 2)
            self._orange_level_button(group, "SLIP", deck, "slip", 1, 3)
            self._orange_level_button(group, "LOOP IN", deck, "loop-in", 2, 0, 2)
            self._orange_level_button(group, "LOOP OUT", deck, "loop-out", 2, 2, 2)
            # The CUE/LOOP CALL arrows traverse saved memory cues/loops; they
            # are separate from the A-H performance pads.
            self._orange_level_button(group, "<", deck, "cue-loop-prev", 3, 0)
            self._orange_level_button(group, ">", deck, "cue-loop-next", 3, 1)
            self._orange_level_button(group, "MEMORY", deck, "memory", 3, 2)
            self._orange_level_button(group, "DELETE", deck, "delete", 3, 3)
            for index, cue in enumerate("ABCDEFGH"):
                self._orange_level_button(group, cue, deck,
                                          f"hotcue-{cue.lower()}",
                                          4 + index // 4, index % 4)
            frame.columnconfigure(deck - 1, weight=1, uniform="deck-group")

    def _orange_level_button(self, parent, label: str, deck: int, control: str,
                             row: int, column: int, span: int = 1):
        """A held ERP level; RBP retains ownership of press/release decoding."""
        button = tk.Button(parent, text=label, bg="#e98724", fg="#101010",
                           activebackground="#ffad54", activeforeground="#101010",
                           relief="raised", borderwidth=1)
        button.grid(row=row, column=column, columnspan=span, padx=2, pady=2,
                    sticky="ew")
        prop = f"deck{deck}-{control}-high"
        button.bind("<ButtonPress-1>",
                    lambda _e: self._level(ERP_PATH, prop, True,
                                           f"Deck {deck} {label}"))
        button.bind("<ButtonRelease-1>",
                    lambda _e: self._level(ERP_PATH, prop, False,
                                           f"Deck {deck} {label}"))

    def _queue_tempo(self, deck: int):
        if deck in self.tempo_after:
            self.after_cancel(self.tempo_after.pop(deck))
        self.tempo_after[deck] = self.after(40, lambda: self._set_tempo(deck))

    def _set_tempo(self, deck: int):
        self.tempo_after.pop(deck, None)
        value = self.tempo[deck].get() / 100.0
        position = round(8192 - value * (8000 if value >= 0 else 7984))
        self.work(lambda: self.qmp.call("qom-set", {"path": ERP_PATH, "property": f"tempo-deck{deck}-position", "value": position}), f"Deck {deck} tempo {value:+.1%}")

    def _tempo_reset_press(self, deck: int, prop: str):
        """Centre one deck's fixture before delivering its reset press edge."""
        pending = self.tempo_after.pop(deck, None)
        if pending:
            self.after_cancel(pending)
        self.tempo[deck].set(0)

        def command():
            self.qmp.call("qom-set", {"path": ERP_PATH,
                                      "property": f"tempo-deck{deck}-position",
                                      "value": 8192})
            return self.qmp.call("qom-set", {"path": ERP_PATH,
                                              "property": prop, "value": True})
        self.work(command, f"Deck {deck} tempo set to 0")

    def _rotary_group(self, parent, row: int):
        frame = ttk.LabelFrame(parent, text="Rotary selector", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=5)
        ttk.Button(frame, text="↺ −1", command=lambda: self._rotary(-1)).grid(row=0, column=0, padx=3)
        self.rotary_count = ttk.Label(frame, text="count: …", anchor="center")
        self.rotary_count.grid(row=0, column=1, padx=12)
        ttk.Button(frame, text="+1 ↻", command=lambda: self._rotary(1)).grid(row=0, column=2, padx=3)
        push = ttk.Button(frame, text="ROTARY PUSH")
        push.grid(row=0, column=3, padx=(12, 3))
        push.bind("<ButtonPress-1>", lambda _e: self._level(MIXER_PATH, "rotary-push-high", True, "Rotary push"))
        push.bind("<ButtonRelease-1>", lambda _e: self._level(MIXER_PATH, "rotary-push-high", False, "Rotary push"))
        frame.columnconfigure(1, weight=1)

    def _usb_group(self, parent, row: int):
        frame = ttk.LabelFrame(parent, text="USB media", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=5)
        ttk.Button(frame, text="Refresh host USB devices", command=self._refresh_physical_usb).grid(
            row=0, column=0, columnspan=2, sticky="e")
        ttk.Label(frame, textvariable=self.physical_usb_text, foreground="#9ad", wraplength=900).grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(2, 6))

        for slot in (1, 2):
            column = ttk.LabelFrame(frame, text=f"USB {slot}", padding=8)
            column.grid(row=2, column=slot - 1, sticky="nsew", padx=(0, 6) if slot == 1 else (6, 0))
            column.columnconfigure(0, weight=1)
            ttk.Label(column, textvariable=self.usb_slot_text[slot], foreground="#9ad").grid(
                row=0, column=0, columnspan=2, sticky="w", pady=(0, 4))
            ttk.Entry(column, textvariable=self.usb_path[slot]).grid(row=1, column=0, sticky="ew", padx=(0, 4))
            ttk.Button(column, text="Choose…", command=lambda s=slot: self._choose_usb(s)).grid(row=1, column=1)
            ttk.Checkbutton(column, text="Read only image", variable=self.usb_read_only[slot]).grid(
                row=2, column=0, sticky="w")
            ttk.Button(column, text="Attach image", command=lambda s=slot: self._attach_usb(s)).grid(
                row=2, column=1, sticky="ew")
            ttk.Separator(column, orient="horizontal").grid(row=3, column=0, columnspan=2, sticky="ew", pady=6)
            picker = ttk.Combobox(column, textvariable=self.physical_usb_choice[slot], state="readonly")
            picker.grid(row=4, column=0, columnspan=2, sticky="ew")
            picker.bind("<<ComboboxSelected>>", lambda _event, s=slot: self._select_physical_usb(s))
            self.physical_usb_picker[slot] = picker
            ttk.Button(column, text="Attach selected physical USB",
                       command=lambda s=slot: self._attach_physical_usb(s)).grid(
                row=5, column=0, columnspan=2, sticky="ew", pady=(4, 0))
            ttk.Button(column, text="Eject USB", command=lambda s=slot: self._eject_usb(s)).grid(
                row=6, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        ttk.Label(frame, text="Physical passthrough requires the privileged QEMU launcher and is never read-only.",
                  foreground="#c88").grid(row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))
        if not self.usb_hotplug:
            ttk.Label(frame, text="Hotplug disabled for this run").grid(row=4, column=0, sticky="w")
        for column in range(2): frame.columnconfigure(column, weight=1, uniform="usb-slots")

    def _level(self, path: str, prop: str, value: bool, label: str):
        self.work(lambda: self.qmp.call("qom-set", {"path": path, "property": prop, "value": value}), f"{label} {'held' if value else 'released'}")

    def _rotary(self, delta: int):
        def command():
            current = self.qmp.call("qom-get", {"path": MIXER_PATH, "property": "rotary-count"})
            count = (int(current) + delta) & 0xffff
            self.qmp.call("qom-set", {"path": MIXER_PATH, "property": "rotary-count", "value": count})
            return {"rotary": count}
        self.work(command, f"Rotary {delta:+d}")

    def _save_usb_paths(self):
        self.settings["usb_images"] = {str(slot): self.usb_path[slot].get()
                                       for slot in (1, 2)}
        try:
            save_settings(self.settings)
        except OSError as error:
            self.status.set(f"Could not save USB paths: {error}")

    def _choose_usb(self, slot: int):
        picked = filedialog.askopenfilename(title="Select raw USB filesystem or disk image")
        if picked:
            self.usb_path[slot].set(picked)
            self._save_usb_paths()

    @staticmethod
    def _physical_usb_label(device: dict[str, object]) -> str:
        location = device["volume"] if device.get("mounted") else f"/dev/{device['disk']} (unmounted)"
        return (f"{location} — {device['name']} ({device['disk']}, "
                f"{device['vendorid']:04x}:{device['productid']:04x})")

    def _refresh_physical_usb(self):
        devices = physical_usb_volumes()
        self.physical_usb_candidates = {
            self._physical_usb_label(device): device for device in devices
        }
        labels = tuple(self.physical_usb_candidates)
        preferred = next((label for label, device in self.physical_usb_candidates.items()
                          if device["volume"] == "/Volumes/A"), labels[0] if labels else "")
        for slot in (1, 2):
            current = self.physical_usb_choice[slot].get()
            choice = current if current in self.physical_usb_candidates else preferred
            self.physical_usb_choice[slot].set(choice)
            self.physical_usb_picker[slot].configure(values=labels)
        if labels:
            mounted = sum(bool(device.get("mounted")) for device in devices)
            self.physical_usb_text.set(
                f"{len(labels)} removable USB device(s) found ({mounted} mounted); select a target USB slot.")
        else:
            self.physical_usb_text.set("No removable USB mass-storage device found")

    def _select_physical_usb(self, slot: int):
        device = self.physical_usb_candidates.get(self.physical_usb_choice[slot].get())
        if device:
            state = "mounted; confirmation will unmount it" if device.get("mounted") \
                else "already unmounted and ready for passthrough"
            self.physical_usb_text.set(f"USB {slot}: selected physical USB ({state}).")

    def _attach_physical_usb(self, slot: int):
        if not self.usb_hotplug:
            self.status.set("Hotplug disabled for this run"); return
        device = self.physical_usb_candidates.get(self.physical_usb_choice[slot].get())
        if not device:
            self.status.set("Select a physical USB first"); return
        raw_device = f"/dev/{device['disk']}"
        if not self.qemu_admin and not os.access(raw_device, os.R_OK):
            messagebox.showerror(
                "USB passthrough needs host permission",
                f"This QEMU process cannot access {raw_device}. On macOS that "
                "also prevents libusb from detaching the storage driver, which "
                "causes an empty guest USB source.\n\n"
                "Use an image attachment, or relaunch QEMU with an explicitly "
                "privileged USB-host setup. The sidecar will not unmount the "
                "drive in this state.")
            return
        if not messagebox.askokcancel(
                "Unmount and pass USB to QEMU",
                f"Unmount the whole physical disk {device['disk']} and give its "
                "USB device directly to QEMU?\n\n"
                "This is not read-only. The host will lose access until it is "
                "ejected from QEMU and remounted.\n\n"
                f"Attach it as virtual USB {slot}?"):
            return
        def command():
            if device.get("mounted"):
                subprocess.run(["diskutil", "unmountDisk", str(device["disk"])],
                               check=True, capture_output=True, text=True)
            if slot in self.usb_slots:
                self._eject_qmp(slot)
            device_id, _, port = USB_SLOT[slot]
            host_id = f"{device_id}-host"
            args = {"driver": "usb-host", "id": host_id, "bus": "usb-bus.1",
                    "port": port, "vendorid": device["vendorid"],
                    "productid": device["productid"]}
            if device["serial"]:
                args["serial"] = device["serial"]
            self.qmp.call("device_add", args)
            self.usb_slots[slot] = ("host", host_id)
        self.work(command, f"Physical USB attached as USB {slot}: {device['name']}")

    def _attach_usb(self, slot: int):
        if not self.usb_hotplug:
            self.status.set("Hotplug disabled for this run"); return
        image = Path(self.usb_path[slot].get()).expanduser().resolve()
        if not image.is_file():
            self.status.set("Error: choose a regular USB image"); return
        def command():
            if slot in self.usb_slots: self._eject_qmp(slot)
            device, node, port = USB_SLOT[slot]
            self.qmp.call("blockdev-add", {"node-name": node, "driver": "raw", "file": {"driver": "file", "filename": str(image)}, "read-only": self.usb_read_only[slot].get()})
            try:
                self.qmp.call("device_add", {"driver": "usb-storage", "id": device, "bus": "usb-bus.1", "port": port, "drive": node})
            except Exception:
                self.qmp.call("blockdev-del", {"node-name": node})
                raise
            self.usb_slots[slot] = ("image", device)
            self._save_usb_paths()
        self.work(command, f"USB {slot} attached: {image.name}")

    def _eject_qmp(self, slot: int):
        kind, device = self.usb_slots[slot]
        _, node, _ = USB_SLOT[slot]
        self.qmp.call("device_del", {"id": device})
        # QEMU removes the device asynchronously; wait briefly before its block node.
        time.sleep(0.15)
        if kind == "image":
            self.qmp.call("blockdev-del", {"node-name": node})
        self.usb_slots.pop(slot, None)

    def _eject_usb(self, slot: int):
        if not self.usb_hotplug:
            self.status.set("Hotplug disabled for this run"); return
        if slot not in self.usb_slots:
            self.status.set(f"USB {slot} is not sidecar-attached"); return
        self.work(lambda: self._eject_qmp(slot), f"USB {slot} ejected")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qmp-port", type=int, required=True)
    parser.add_argument("--usb-hotplug", choices=("on", "off"), default="on")
    parser.add_argument("--erp-tempo", choices=("on", "off"), default="on")
    parser.add_argument("--usb-default", help="Preselect the launcher USB image in the sidecar")
    parser.add_argument("--usb-a1-pre-attached", action="store_true",
                        help="USB 1 was already attached by the launcher")
    parser.add_argument("--qemu-admin", choices=("on", "off"), default="off",
                        help="Whether the runner started QEMU through sudo")
    args = parser.parse_args()
    Controls(args.qmp_port, args.usb_hotplug == "on", args.erp_tempo == "on",
             args.usb_default, args.usb_a1_pre_attached,
             args.qemu_admin == "on").mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
