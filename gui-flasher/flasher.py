#!/usr/bin/env python3
"""
flasher.py -- a small GUI to erase, merge and flash the ESP32-P4 photo frame.

Deliberately independent of the ESP-IDF build system: it never sources
tools/env.sh or touches the IDF Python venv. It shells out directly to the
`esptool` CLI, which already carries its own bundled pyserial, and reads a
build directory's `flash_args` file for the chip offsets and file list --
the same file `idf.py flash` itself generates and consumes -- so this tool
never keeps its own copy of the partition layout.

Flashing always goes through `esptool merge-bin` first: bootloader,
partition table and app are combined into one file (`merged-firmware.bin`,
written into the selected build directory) and THAT is what gets flashed, at
one offset. This is a deliberate choice, not an oversight: the merged file's
byte range spans the NVS and phy_init partitions too, so writing it erases
whatever WiFi credentials and settings.c state were saved on the device --
every flash is a clean slate. If you ever want a routine update that
preserves those instead, that needs writing bootloader/partition-table/app
back to their own three separate offsets, which is NOT what this tool does.

Usage:  python3 flasher.py
"""
from __future__ import annotations

import glob
import queue
import shlex
import shutil
import subprocess
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk
from typing import Callable

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build"

BAUD_OPTIONS = ["115200", "230400", "460800", "921600"]
DEFAULT_BAUD = "460800"          # what this project already flashes at

# esp32p4 explicit and default: sdkconfig.defaults pins this project to it,
# and merging (unlike erasing or flashing) has no device to probe -- esptool
# flatly refuses "auto" for merge-bin since there's nothing connected to ask.
# "auto" stays available for Erase/Flash, where a board really is attached;
# the rest of the list is here only in case this tool is pointed at another
# board someday.
CHIP_OPTIONS = ["esp32p4", "auto", "esp32", "esp32s2", "esp32s3",
                "esp32c3", "esp32c6", "esp32h2"]
DEFAULT_CHIP = "esp32p4"

# Written into the SELECTED BUILD DIRECTORY (already git-ignored, since it
# lives under a directory literally named "build") every time Merge or Flash
# runs. This is deliberately not a temp file: it is the single distributable
# artifact the whole feature exists for, so it needs to still be sitting there
# afterward for the user to hand to someone else.
MERGED_FILENAME = "merged-firmware.bin"


# ---------------------------------------------------------------------------
# Pure logic, kept separate from the UI so it can be exercised without a
# display (`python3 -c "import flasher"` runs none of this at import time,
# since it all lives behind the __main__ guard except these free functions).
# ---------------------------------------------------------------------------
def find_esptool() -> str | None:
    """Prefer the plain `esptool` on PATH -- the `esptool.py` alias works too
    but prints its own deprecation warning on every single invocation."""
    found = shutil.which("esptool")
    if found:
        return found
    for candidate in (Path.home() / ".local/bin/esptool",
                      Path.home() / ".local/bin/esptool.py"):
        if candidate.is_file():
            return str(candidate)
    return None


def list_serial_ports() -> list[str]:
    """macOS-only by design (this tool targets this Mac). USB-serial and
    USB-JTAG adapters enumerate as /dev/cu.usbserial-* or /dev/cu.usbmodem-*;
    those are listed first since they're what a board actually shows up as."""
    ports = glob.glob("/dev/cu.*")

    def rank(p: str) -> tuple[int, str]:
        name = Path(p).name
        likely = name.startswith(("cu.usbserial", "cu.usbmodem"))
        return (0 if likely else 1, name)

    return sorted(ports, key=rank)


def normalize_offset(text: str) -> str | None:
    """Accept "0x2000", "0X2000" or bare "2000" and return a canonical
    "0x..." string, or None if it isn't valid hex at all."""
    text = text.strip()
    if not text:
        return None
    body = text[2:] if text.lower().startswith("0x") else text
    try:
        return f"0x{int(body, 16):x}"
    except ValueError:
        return None


def normalize_flag(token: str) -> str:
    """flash_args uses esptool's old snake_case flags (--flash_mode). They
    still work but print a deprecation warning per esptool 5.3.1, and esptool
    says the underscore forms will eventually be removed -- so every flag
    token is rewritten to the current dashed form before use."""
    if token.startswith("--"):
        return token.replace("_", "-")
    return token


class FlashArgsError(Exception):
    pass


def parse_flash_args(build_dir: Path):
    """Parse a build directory's `flash_args` -- the file idf.py itself
    generates and flashes from. Returns (flash_options, entries) where
    flash_options is a list of normalized CLI tokens (e.g.
    ["--flash-mode", "dio", ...]) and entries is a list of (offset, Path)
    with every path resolved and confirmed to exist.

    Raises FlashArgsError with a message fit to show the user directly.
    """
    path = build_dir / "flash_args"
    if not build_dir.is_dir():
        raise FlashArgsError(f"{build_dir} is not a directory")
    if not path.is_file():
        raise FlashArgsError(f"no flash_args in {build_dir} -- run a build first")

    lines = [ln.strip() for ln in path.read_text().splitlines() if ln.strip()]
    if not lines:
        raise FlashArgsError(f"{path} is empty")

    flash_options = [normalize_flag(tok) for tok in shlex.split(lines[0])]

    entries: list[tuple[str, Path]] = []
    for lineno, line in enumerate(lines[1:], start=2):
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise FlashArgsError(f"{path}:{lineno}: expected '<offset> <file>', got {line!r}")
        offset, rel = parts
        file_path = (build_dir / rel).resolve()
        if not file_path.is_file():
            raise FlashArgsError(f"{path}:{lineno}: {file_path} does not exist "
                                 f"-- the build directory may be stale, try rebuilding")
        entries.append((offset, file_path))

    if not entries:
        raise FlashArgsError(f"{path} lists no files")

    return flash_options, entries


# ---------------------------------------------------------------------------
# UI -- dark theme, matching the "Firmware Loading Tab Separation" design.
# Purely presentational: every color/spacing constant below, and every method
# in the "construction" section of FlasherApp. Nothing past that point (the
# firmware/port/command-building/process-execution logic) changed at all.
# ---------------------------------------------------------------------------
BG            = "#242220"   # window / tab-content background
PANEL_BG      = "#2c2a27"   # card fill
BORDER        = "#46423d"   # card border, hairline under the tab row
INPUT_BG      = "#3a3733"   # entry / combobox / secondary-button fill
INPUT_BORDER  = "#4d4941"
INPUT_HOVER   = "#46423d"
TABLE_HEAD_BG = "#1c1b19"
TABLE_ROW_BG  = "#181715"
TEXT          = "#eae7e2"   # primary text
TEXT_DIM      = "#a8a29b"   # labels, secondary text
TEXT_FAINT    = "#7d786f"   # hints, disabled text
TABLE_TEXT    = "#d7d2cb"   # table rows / log
LOG_BG        = "#121110"
PREVIEW_FG    = "#e8b985"   # the "$ ..." command line specifically
ERROR_FG      = "#ff8577"   # status_var's red, lightened to read on a dark bg
ACCENT        = "#ff8a3d"
ACCENT_HOVER  = "#ffa15e"
ACCENT_TEXT   = "#2a1608"   # dark text on the solid accent fill
ACCENT_GHOST  = "#3a2417"   # accent-outline button's hover fill

MONO_FONT   = ("Menlo", 12)
UI_FONT     = ("TkDefaultFont", 12)
HEADING_FONT = ("TkDefaultFont", 13, "bold")
BOLD_FONT   = ("TkDefaultFont", 12, "bold")

TAB_DEFS = [("multi", "3-Bin Flash"), ("merged", "Merged Image")]


class FlasherApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ESP32-P4 Photo Frame Flasher")
        # An explicit starting geometry, not just minsize: everything below is
        # packed into a scrollable canvas now, so the window's actual size no
        # longer has to equal the sum of every section's height to show all of
        # it -- it can be smaller than the content, with the rest reachable by
        # scrolling, which is the whole point of adding the canvas at all.
        self.geometry("920x700")
        self.minsize(760, 480)

        self.build_dir: Path = DEFAULT_BUILD_DIR
        self.flash_options: list[str] = []
        self.entries: list[tuple[str, Path]] = []

        self.esptool_path_var = tk.StringVar(value=find_esptool() or "")
        self.build_dir_var = tk.StringVar(value=str(self.build_dir))
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=DEFAULT_BAUD)
        self.chip_var = tk.StringVar(value=DEFAULT_CHIP)
        self.status_var = tk.StringVar(value="")
        self.preview_var = tk.StringVar(value="")

        # A separate, independent path: flash an ALREADY-MERGED file directly,
        # with no build directory or flash_args involved at all. Covers
        # reflashing the same image to several boards without re-merging each
        # time, and flashing a file someone else handed you.
        self.existing_file_var = tk.StringVar(value="")
        self.existing_offset_var = tk.StringVar(value="0x2000")

        self._log_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self._proc: subprocess.Popen | None = None
        self._action_buttons: list[tk.Widget] = []
        self._pending_on_success: Callable[[], None] | None = None

        self._build_ui()
        self._refresh_ports()
        self._reload_firmware()
        self._poll_log_queue()

    # -- construction --------------------------------------------------
    def _setup_style(self):
        """Switches to ttk's "clam" theme and restyles everything from there.
        macOS's default "aqua" theme renders most widgets natively and
        ignores background/foreground overrides -- "clam" is drawn entirely
        by Tk itself, which is what makes a real dark theme possible at all."""
        self.configure(bg=BG)
        style = ttk.Style(self)
        style.theme_use("clam")

        style.configure(".", background=BG, foreground=TEXT, font=UI_FONT)
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=TEXT)
        style.configure("Dim.TLabel", background=BG, foreground=TEXT_DIM)
        style.configure("Error.TLabel", background=BG, foreground=ERROR_FG)
        style.configure("Heading.TLabel", background=BG, foreground=TEXT,
                        font=HEADING_FONT)

        # A card: the bordered container every section's content sits in.
        # CardRow is the same fill with no border, for a plain row nested
        # inside a card (so it doesn't show as a second, smaller box).
        style.configure("Card.TFrame", background=PANEL_BG, bordercolor=BORDER,
                        relief="solid", borderwidth=1)
        style.configure("CardRow.TFrame", background=PANEL_BG)

        style.configure("TEntry", fieldbackground=INPUT_BG, foreground=TEXT,
                        insertcolor=TEXT, bordercolor=INPUT_BORDER, font=MONO_FONT,
                        padding=6)
        style.map("TEntry", foreground=[("readonly", TEXT_DIM)])

        style.configure("TCombobox", fieldbackground=INPUT_BG, background=INPUT_BG,
                        foreground=TEXT, arrowcolor=TEXT_DIM, bordercolor=INPUT_BORDER,
                        font=MONO_FONT, padding=5)
        style.map("TCombobox",
                 fieldbackground=[("readonly", INPUT_BG), ("disabled", INPUT_BG)],
                 foreground=[("readonly", TEXT), ("disabled", TEXT_FAINT)])
        # The dropdown listbox is a plain Tk widget, styled separately.
        self.option_add("*TCombobox*Listbox.background", INPUT_BG)
        self.option_add("*TCombobox*Listbox.foreground", TEXT)
        self.option_add("*TCombobox*Listbox.selectBackground", ACCENT)
        self.option_add("*TCombobox*Listbox.selectForeground", ACCENT_TEXT)

        # Three button tiers, matching the design's hierarchy: neutral
        # actions (Browse, Merge, Refresh, Erase, Cancel), the one primary
        # action per tab (solid accent fill), and the doubly-destructive
        # combo action (accent outline -- important, but not THE primary
        # action, so it doesn't compete with it for attention).
        style.configure("TButton", background=INPUT_BG, foreground=TEXT,
                        bordercolor=INPUT_BORDER, padding=(14, 8), font=UI_FONT)
        style.map("TButton", background=[("active", INPUT_HOVER), ("disabled", BG)],
                 foreground=[("disabled", TEXT_FAINT)])
        style.configure("Secondary.TButton", background=INPUT_BG, foreground=TEXT,
                        bordercolor=INPUT_BORDER, padding=(14, 8), font=UI_FONT)
        style.map("Secondary.TButton",
                 background=[("active", INPUT_HOVER), ("disabled", BG)],
                 foreground=[("disabled", TEXT_FAINT)])
        style.configure("Accent.TButton", background=ACCENT, foreground=ACCENT_TEXT,
                        bordercolor=ACCENT, padding=(14, 8), font=BOLD_FONT)
        style.map("Accent.TButton",
                 background=[("active", ACCENT_HOVER), ("disabled", INPUT_BG)],
                 foreground=[("disabled", TEXT_FAINT)])
        style.configure("AccentOutline.TButton", background=BG, foreground=ACCENT,
                        bordercolor=ACCENT, padding=(14, 8), font=BOLD_FONT)
        style.map("AccentOutline.TButton",
                 background=[("active", ACCENT_GHOST), ("disabled", BG)],
                 foreground=[("disabled", TEXT_FAINT)])

        style.configure("Treeview", background=TABLE_ROW_BG, fieldbackground=TABLE_ROW_BG,
                        foreground=TABLE_TEXT, bordercolor=BORDER, borderwidth=0,
                        font=MONO_FONT, rowheight=24)
        style.configure("Treeview.Heading", background=TABLE_HEAD_BG, foreground=TEXT_DIM,
                        bordercolor=BORDER, font=("TkDefaultFont", 10, "bold"))
        style.map("Treeview", background=[("selected", ACCENT)],
                 foreground=[("selected", ACCENT_TEXT)])

        style.configure("TProgressbar", background=ACCENT, troughcolor=INPUT_BG,
                        bordercolor=BORDER, lightcolor=ACCENT, darkcolor=ACCENT)

        style.configure("Vertical.TScrollbar", background=INPUT_BG, troughcolor=BG,
                        bordercolor=BORDER, arrowcolor=TEXT_DIM, relief="flat")
        style.map("Vertical.TScrollbar", background=[("active", INPUT_HOVER)])

    def _build_scroll_container(self) -> ttk.Frame:
        """Every section is packed into `content`, not `self` directly.
        `content` sits inside a Canvas that's the actual direct child of the
        window, so the window's own size is decoupled from the sum of every
        section's height -- the earlier, everything-packed-on-self layout had
        no way to be shorter than its content, which was the entire "needs
        too much height, and it is not scrollable" complaint."""
        outer = ttk.Frame(self)
        outer.pack(fill="both", expand=True)

        canvas = tk.Canvas(outer, bg=BG, highlightthickness=0, bd=0)
        vbar = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vbar.set)
        canvas.pack(side="left", fill="both", expand=True)
        vbar.pack(side="right", fill="y")

        content = ttk.Frame(canvas)
        content_id = canvas.create_window((0, 0), window=content, anchor="nw")

        def on_content_configure(_event=None):
            canvas.configure(scrollregion=canvas.bbox("all"))
        content.bind("<Configure>", on_content_configure)

        def on_canvas_configure(event):
            # Content stretches to the canvas's width, so it never scrolls
            # sideways -- only vertically, which is the actual problem here.
            canvas.itemconfig(content_id, width=event.width)
        canvas.bind("<Configure>", on_canvas_configure)

        # Mouse-wheel scrolling for the window as a whole. bind_all is
        # deliberately broad -- it's how the wheel reaches the canvas no
        # matter which child widget the pointer happens to be over (Tk does
        # not bubble <MouseWheel> up a widget's ancestors on its own) -- but
        # that means the Log and the firmware table, which need to scroll
        # THEMSELVES, must explicitly override it. See _exempt_from_page_scroll.
        def on_mousewheel(event):
            canvas.yview_scroll(-1 * event.delta, "units")
        canvas.bind_all("<MouseWheel>", on_mousewheel)

        return content

    def _exempt_from_page_scroll(self, widget):
        """Gives `widget` back its own mouse-wheel scrolling and stops that
        same wheel event from ALSO scrolling the outer page underneath it --
        without this, hovering over the Log or the firmware table while
        scrolling would move both at once. Returning "break" here is what
        stops the bind_all handler in _build_scroll_container from also
        firing for this event."""
        def handler(event):
            widget.yview_scroll(-1 * event.delta, "units")
            return "break"
        widget.bind("<MouseWheel>", handler)

    def _build_tabs(self, parent):
        """A hand-rolled tab bar rather than ttk.Notebook: Notebook's own tab
        styling fights "clam" in exactly the ways that matter here (a flat,
        text-plus-underline look with no boxed tab), so plain Labels with a
        click binding and a colored Frame underneath as the underline give
        precise control for very little code."""
        bar = ttk.Frame(parent)
        bar.pack(fill="x", padx=8, pady=(10, 0))

        self._tab_widgets: dict[str, tuple[tk.Label, tk.Frame]] = {}
        for i, (key, label_text) in enumerate(TAB_DEFS):
            col = ttk.Frame(bar)
            col.grid(row=0, column=i, sticky="w", padx=(0 if i == 0 else 24, 0))
            lbl = tk.Label(col, text=label_text, bg=BG, fg=TEXT_DIM,
                          font=HEADING_FONT, cursor="hand2", bd=0, highlightthickness=0)
            lbl.pack(anchor="w", pady=(0, 10))
            lbl.bind("<Button-1>", lambda _e, k=key: self._set_tab(k))
            underline = tk.Frame(col, bg=BG, height=2, bd=0, highlightthickness=0)
            underline.pack(fill="x")
            self._tab_widgets[key] = (lbl, underline)

        hairline = tk.Frame(parent, bg=BORDER, height=1, bd=0, highlightthickness=0)
        hairline.pack(fill="x", padx=8)

    def _set_tab(self, key: str):
        self._active_tab = key
        for k, (lbl, underline) in self._tab_widgets.items():
            active = k == key
            lbl.configure(fg=ACCENT if active else TEXT_DIM)
            underline.configure(bg=ACCENT if active else BG)
        for k, frame in self._tab_frames.items():
            if k == key:
                frame.pack(fill="both", expand=True)
            else:
                frame.pack_forget()

    def _card(self, parent, title: str, desc: str | None = None) -> ttk.Frame:
        """A heading (+ optional one-line description) above a bordered,
        filled container, matching the design's "label above a card" pattern
        -- as opposed to ttk.LabelFrame's native inset-title-on-the-border
        look, which the design doesn't use anywhere. Returns the card's
        content frame, padded and ready for children."""
        ttk.Label(parent, text=title, style="Heading.TLabel").pack(
            anchor="w", pady=(0, 2 if desc else 10))
        if desc:
            ttk.Label(parent, text=desc, style="Dim.TLabel",
                     wraplength=760, justify="left").pack(anchor="w", pady=(0, 10))
        box = ttk.Frame(parent, style="Card.TFrame", padding=14)
        box.pack(fill="x")
        return box

    def _build_tab_multi(self, parent):
        """Tab 1: the build-directory-driven pipeline (Merge / Flash / Erase
        && Flash). Identical widgets and StringVars to before -- only where
        they're built, and how they're styled, has changed."""
        card = self._card(parent, "Firmware build")

        row = ttk.Frame(card, style="CardRow.TFrame")
        row.pack(fill="x", pady=(0, 10))
        ttk.Entry(row, textvariable=self.build_dir_var, state="readonly").pack(
            side="left", fill="x", expand=True, padx=(0, 8))
        ttk.Button(row, text="Browse…", style="Secondary.TButton",
                  command=self._browse_build_dir).pack(side="left")

        self.tree = ttk.Treeview(card, columns=("offset", "file"),
                                 show="headings", height=4)
        self.tree.heading("offset", text="Offset")
        self.tree.heading("file", text="File")
        self.tree.column("offset", width=100, anchor="w", stretch=False)
        self.tree.column("file", anchor="w")
        self.tree.pack(fill="x", expand=True)

        ttk.Label(parent, textvariable=self.status_var, style="Error.TLabel").pack(
            anchor="w", pady=(8, 0))

        btn_row = ttk.Frame(parent)
        btn_row.pack(fill="x", pady=(14, 0))
        # Merge is neutral and first: it's the only action here that touches
        # no device at all, so it isn't styled as urgent the way Flash is.
        self._add_action(btn_row, "Merge Firmware", self._on_merge,
                         style="Secondary.TButton")
        self._add_action(btn_row, "Flash Firmware", self._on_flash,
                         style="Accent.TButton")
        self._add_action(btn_row, "Erase && Flash", self._on_erase_and_flash,
                         style="AccentOutline.TButton")

    def _build_tab_merged(self, parent):
        """Tab 2: flash an already-produced file directly, no build
        directory involved."""
        card = self._card(
            parent, "Flash a merged image",
            desc="Skip the merge step — for reflashing several boards, "
                "or a file someone else sent you.")

        row = ttk.Frame(card, style="CardRow.TFrame")
        row.pack(fill="x")
        ttk.Entry(row, textvariable=self.existing_file_var).pack(
            side="left", fill="x", expand=True, padx=(0, 12))
        ttk.Button(row, text="Browse…", style="Secondary.TButton",
                  command=self._browse_existing_file).pack(side="left", padx=(0, 16))
        ttk.Label(row, text="Offset:", background=PANEL_BG, foreground=TEXT_DIM).pack(
            side="left", padx=(0, 6))
        ttk.Entry(row, textvariable=self.existing_offset_var, width=10).pack(
            side="left", padx=(0, 16))
        self._add_action(row, "Flash This File", self._on_flash_existing,
                         style="Accent.TButton")

    def _build_connection(self, parent):
        card = self._card(parent, "Connection")

        row1 = ttk.Frame(card, style="CardRow.TFrame")
        row1.pack(fill="x", pady=(0, 12))
        ttk.Label(row1, text="Port:", background=PANEL_BG, foreground=TEXT_DIM,
                 width=8).pack(side="left")
        # NOT readonly: the dropdown lists what's currently plugged in, but a
        # port that isn't enumerating right now (or a path this glob doesn't
        # match) still needs to be typeable.
        self.port_combo = ttk.Combobox(row1, textvariable=self.port_var, width=26)
        self.port_combo.pack(side="left", padx=(0, 10))
        ttk.Button(row1, text="Refresh", style="Secondary.TButton",
                  command=self._refresh_ports).pack(side="left", padx=(0, 10))
        ttk.Label(row1, text="(or type a path)", background=PANEL_BG,
                 foreground=TEXT_FAINT).pack(side="left")

        row2 = ttk.Frame(card, style="CardRow.TFrame")
        row2.pack(fill="x", pady=(0, 12))
        ttk.Label(row2, text="Baud:", background=PANEL_BG, foreground=TEXT_DIM,
                 width=8).pack(side="left")
        ttk.Combobox(row2, textvariable=self.baud_var, values=BAUD_OPTIONS,
                    width=12, state="readonly").pack(side="left", padx=(0, 24))
        ttk.Label(row2, text="Chip:", background=PANEL_BG,
                 foreground=TEXT_DIM).pack(side="left", padx=(0, 10))
        ttk.Combobox(row2, textvariable=self.chip_var, values=CHIP_OPTIONS,
                    width=12, state="readonly").pack(side="left")

        row3 = ttk.Frame(card, style="CardRow.TFrame")
        row3.pack(fill="x")
        ttk.Label(row3, text="esptool:", background=PANEL_BG, foreground=TEXT_DIM,
                 width=8).pack(side="left")
        ttk.Entry(row3, textvariable=self.esptool_path_var).pack(
            side="left", fill="x", expand=True, padx=(0, 10))
        ttk.Button(row3, text="Browse…", style="Secondary.TButton",
                  command=self._browse_esptool).pack(side="left")

        for var in (self.port_var, self.baud_var, self.chip_var, self.esptool_path_var):
            var.trace_add("write", lambda *_: self._update_preview())

    def _build_ui(self):
        self._setup_style()
        content = self._build_scroll_container()

        self._build_tabs(content)

        self.tab_multi_frame = ttk.Frame(content, padding=(8, 10, 8, 6))
        self.tab_merged_frame = ttk.Frame(content, padding=(8, 10, 8, 6))
        self._build_tab_multi(self.tab_multi_frame)
        self._build_tab_merged(self.tab_merged_frame)
        self._tab_frames = {"multi": self.tab_multi_frame, "merged": self.tab_merged_frame}
        self._set_tab("multi")

        conn_wrap = ttk.Frame(content, padding=(8, 0, 8, 6))
        conn_wrap.pack(fill="x")
        self._build_connection(conn_wrap)

        # Shared regardless of tab: erasing needs no firmware file selected
        # at all, and Cancel applies to whatever is currently running.
        shared_row = ttk.Frame(content, padding=(8, 0, 8, 6))
        shared_row.pack(fill="x")
        self._add_action(shared_row, "Erase Flash", self._on_erase,
                         style="Secondary.TButton")
        self.cancel_btn = ttk.Button(shared_row, text="Cancel", command=self._on_cancel,
                                     state="disabled", style="Secondary.TButton")
        self.cancel_btn.pack(side="left", padx=(0, 10))

        self.progress = ttk.Progressbar(content, mode="indeterminate")
        self.progress.pack(fill="x", padx=8)

        ttk.Label(content, textvariable=self.preview_var, foreground=PREVIEW_FG,
                 font=("Menlo", 10)).pack(fill="x", padx=8, pady=(6, 0), anchor="w")

        # height=8 rather than the original 16: the window itself scrolls now,
        # so the log no longer needs to be tall enough to show everything by
        # itself -- it still grows into any extra space via fill/expand, this
        # just lowers what it asks for by default.
        log_wrap = ttk.Frame(content, padding=(8, 6, 8, 8))
        log_wrap.pack(fill="both", expand=True)
        ttk.Label(log_wrap, text="Log", style="Heading.TLabel").pack(
            anchor="w", pady=(0, 6))
        self.log = scrolledtext.ScrolledText(
            log_wrap, height=8, state="disabled", font=("Menlo", 11), wrap="word",
            bg=LOG_BG, fg=TABLE_TEXT, insertbackground=TEXT, relief="flat",
            borderwidth=0, highlightthickness=1, highlightbackground=BORDER,
            highlightcolor=BORDER)
        self.log.pack(fill="both", expand=True)

        # The log and the firmware table scroll themselves; without this,
        # hovering over either while using the wheel would also scroll the
        # whole page underneath them at the same time.
        self._exempt_from_page_scroll(self.log)
        self._exempt_from_page_scroll(self.tree)

    def _add_action(self, parent, label, command, style="Secondary.TButton"):
        btn = ttk.Button(parent, text=label, command=command, style=style)
        btn.pack(side="left", padx=(0, 10))
        self._action_buttons.append(btn)
        return btn

    # -- firmware -------------------------------------------------------
    def _browse_build_dir(self):
        chosen = filedialog.askdirectory(
            title="Select a build directory (containing flash_args)",
            initialdir=str(self.build_dir if self.build_dir.is_dir() else PROJECT_ROOT))
        if chosen:
            self.build_dir = Path(chosen)
            self.build_dir_var.set(str(self.build_dir))
            self._reload_firmware()

    def _reload_firmware(self):
        self.tree.delete(*self.tree.get_children())
        try:
            self.flash_options, self.entries = parse_flash_args(self.build_dir)
        except FlashArgsError as e:
            self.flash_options, self.entries = [], []
            self.status_var.set(str(e))
        else:
            self.status_var.set("")
            for offset, file_path in self.entries:
                try:
                    rel = file_path.relative_to(self.build_dir)
                except ValueError:
                    rel = file_path
                self.tree.insert("", "end", values=(offset, str(rel)))
            # A merged file may already be sitting here from an earlier
            # session. Point "Flash an existing image" at it, but only when
            # that field is blank or was itself last set to a merged-firmware
            # -- never clobber a file the user deliberately browsed to.
            merged = self._merged_path()
            current = self.existing_file_var.get().strip()
            if merged.is_file() and (not current or Path(current).name == MERGED_FILENAME):
                self._fill_existing_from(merged, self._target_offset())
        self._update_preview()

    # -- ports / esptool --------------------------------------------------
    def _refresh_ports(self):
        ports = list_serial_ports()
        self.port_combo["values"] = ports
        if ports and self.port_var.get() not in ports:
            self.port_var.set(ports[0])
        self._update_preview()

    def _browse_existing_file(self):
        start_dir = self.build_dir if self.build_dir.is_dir() else PROJECT_ROOT
        chosen = filedialog.askopenfilename(
            title="Select a merged firmware image to flash",
            initialdir=str(start_dir),
            filetypes=[("Binary firmware", "*.bin"), ("All files", "*.*")])
        if chosen:
            self.existing_file_var.set(chosen)

    def _browse_esptool(self):
        chosen = filedialog.askopenfilename(title="Select the esptool executable",
                                            initialdir=str(Path.home() / ".local/bin"))
        if chosen:
            self.esptool_path_var.set(chosen)

    # -- command construction --------------------------------------------
    def _merged_path(self) -> Path:
        return self.build_dir / MERGED_FILENAME

    def _target_offset(self) -> str | None:
        """The lowest offset among the parsed entries -- normally the
        bootloader's 0x2000. This is both merge-bin's --target-offset (so the
        merged file's own byte 0 IS flash address 0x2000, rather than padding
        in an unused 0x0-0x2000 prefix) and the single address write-flash
        later writes that merged file at."""
        if not self.entries:
            return None
        return min(self.entries, key=lambda e: int(e[0], 16))[0]

    def _require_esptool(self) -> str | None:
        esptool = self.esptool_path_var.get().strip()
        if not esptool:
            messagebox.showerror("No esptool", "Set the path to the esptool executable.")
            return None
        return esptool

    def _base_args(self) -> list[str] | None:
        """chip + port + baud, for the two operations that actually touch a
        connected board (erase-flash, write-flash)."""
        esptool = self._require_esptool()
        if esptool is None:
            return None
        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("No port", "Choose or type a serial port.")
            return None
        return [esptool, "--chip", self.chip_var.get(),
                "--port", port, "--baud", self.baud_var.get()]

    def _erase_args(self) -> list[str] | None:
        base = self._base_args()
        return base + ["erase-flash"] if base else None

    def _merge_args(self) -> list[str] | None:
        """No --port/--baud: merging never touches a device, which is also
        why it needs a CONCRETE --chip (not "auto") -- there is nothing
        connected here for esptool to probe to resolve "auto" itself."""
        esptool = self._require_esptool()
        if esptool is None:
            return None
        if not self.entries:
            messagebox.showerror("No firmware", self.status_var.get() or
                                 "Select a build directory with a valid flash_args.")
            return None
        chip = self.chip_var.get()
        if chip == "auto":
            messagebox.showerror(
                "Chip required",
                "Merging needs a specific chip, not \"auto\" -- there's no "
                "device here for esptool to detect it from. Pick esp32p4.")
            return None

        target_offset = self._target_offset()
        sorted_entries = sorted(self.entries, key=lambda e: int(e[0], 16))
        cmd = [esptool, "--chip", chip, "merge-bin",
              "-o", str(self._merged_path())] + self.flash_options + \
              ["--target-offset", target_offset]
        for offset, file_path in sorted_entries:
            cmd += [offset, str(file_path)]
        return cmd

    def _write_merged_args(self) -> list[str] | None:
        """write-flash for the merged file this tool just produced. No
        --flash-mode/--flash-freq/--flash-size needed here: merge-bin already
        baked those into the bootloader header inside the merged image."""
        base = self._base_args()
        target_offset = self._target_offset()
        if base is None or target_offset is None:
            return None
        return base + ["write-flash", target_offset, str(self._merged_path())]

    def _existing_file_args(self) -> list[str] | None:
        """write-flash for a file the user pointed at directly -- no build
        directory, no flash_args, no merge step. Just an offset and a file."""
        base = self._base_args()
        if base is None:
            return None
        raw_path = self.existing_file_var.get().strip()
        if not raw_path:
            messagebox.showerror("No file", "Choose the firmware image to flash.")
            return None
        file_path = Path(raw_path)
        if not file_path.is_file():
            messagebox.showerror("File not found", f"{file_path} does not exist.")
            return None
        offset = normalize_offset(self.existing_offset_var.get())
        if offset is None:
            messagebox.showerror(
                "Bad offset",
                f"{self.existing_offset_var.get()!r} isn't a valid offset "
                "-- expected something like 0x2000.")
            return None
        return base + ["write-flash", offset, str(file_path)]

    def _update_preview(self):
        cmd = self._base_args()
        if cmd is None:
            self.preview_var.set("")
            return
        self.preview_var.set("$ " + shlex.join(cmd) +
                             " <erase-flash | write-flash <merged-firmware.bin>>")

    # -- actions ------------------------------------------------------------
    def _fill_existing_from(self, merged_path: Path, target_offset: str | None):
        """On success, point "Flash an existing merged image" at the file
        that was just produced -- e.g. Merge once, then Flash This File to
        several boards in a row with no re-merging in between. Captured at
        command-construction time rather than re-read from self.build_dir
        here, since Browse... for the build directory isn't blocked while a
        job runs and could otherwise point this at the wrong build's file."""
        self.existing_file_var.set(str(merged_path))
        if target_offset:
            self.existing_offset_var.set(target_offset)

    def _on_merge(self):
        """Produces merged-firmware.bin only -- no port needed, no device
        touched. This is the "just give me a file to hand to someone else"
        action, and it also doubles as a dry run: if this succeeds, Flash
        Firmware's own merge step will too."""
        cmd = self._merge_args()
        if not cmd:
            return
        merged_path, target_offset = self._merged_path(), self._target_offset()
        self._run(cmd, "Merge",
                 on_success=lambda: self._fill_existing_from(merged_path, target_offset))

    def _on_flash_existing(self):
        file_path = self.existing_file_var.get().strip()
        offset = self.existing_offset_var.get().strip()
        if not messagebox.askyesno(
                "Flash existing file",
                f"This writes\n\n  {file_path}\n\ndirectly to offset {offset} "
                "on the board. Make sure this is the right image and offset "
                "for this board -- an image built for a different offset or "
                "chip can leave it unable to boot until reflashed correctly."
                "\n\nContinue?"):
            return
        cmd = self._existing_file_args()
        if cmd:
            self._run(cmd, "Flash")

    def _on_erase(self):
        if not messagebox.askyesno(
                "Erase flash",
                "This erases the ENTIRE flash chip and cannot be undone.\n\n"
                "Continue?"):
            return
        cmd = self._erase_args()
        if cmd:
            self._run(cmd, "Erase")

    def _on_flash(self):
        if not messagebox.askyesno(
                "Flash firmware",
                "This writes a merged bootloader+partition-table+app image "
                "that also covers the NVS and phy_init regions, so it "
                "erases any WiFi credentials and saved settings on the "
                "device -- every flash starts from a clean slate.\n\n"
                "Continue?"):
            return
        # Each _..._args() call can itself pop up an error dialog (missing
        # port, missing esptool, ...); stopping at the first None avoids
        # showing that same dialog two or three times over for one problem.
        merge_cmd = self._merge_args()
        if merge_cmd is None:
            return
        flash_cmd = self._write_merged_args()
        if flash_cmd is None:
            return
        merged_path, target_offset = self._merged_path(), self._target_offset()
        self._run_sequence(
            [("Merge", merge_cmd), ("Flash", flash_cmd)],
            on_success=lambda: self._fill_existing_from(merged_path, target_offset))

    def _on_erase_and_flash(self):
        if not messagebox.askyesno(
                "Erase && Flash",
                "This erases the ENTIRE flash chip, then writes the merged "
                "firmware image. Neither step can be undone, and the device "
                "will need WiFi and settings set up again afterward.\n\n"
                "Continue?"):
            return
        erase_cmd = self._erase_args()
        if erase_cmd is None:
            return
        merge_cmd = self._merge_args()
        if merge_cmd is None:
            return
        flash_cmd = self._write_merged_args()
        if flash_cmd is None:
            return
        merged_path, target_offset = self._merged_path(), self._target_offset()
        self._run_sequence(
            [("Erase", erase_cmd), ("Merge", merge_cmd), ("Flash", flash_cmd)],
            on_success=lambda: self._fill_existing_from(merged_path, target_offset))

    def _on_cancel(self):
        if self._proc is not None:
            self._append_log(f"\n--- cancelling (pid {self._proc.pid}) ---\n")
            self._proc.terminate()

    # -- process execution -------------------------------------------------
    def _set_running(self, running: bool):
        state = "disabled" if running else "normal"
        for btn in self._action_buttons:
            btn.configure(state=state)
        self.cancel_btn.configure(state="normal" if running else "disabled")
        if running:
            self.progress.start(12)
        else:
            self.progress.stop()

    def _run(self, cmd: list[str], label: str,
            on_success: Callable[[], None] | None = None):
        self._run_sequence([(label, cmd)], on_success=on_success)

    def _run_sequence(self, steps: list[tuple[str, list[str]]],
                      on_success: Callable[[], None] | None = None):
        """Run one or more commands back to back, stopping at the first
        non-zero exit so a failed erase never proceeds to flash. on_success
        runs on the main thread once every step has exited zero."""
        self._pending_on_success = on_success
        self._set_running(True)

        def worker():
            for label, cmd in steps:
                self._log_queue.put(("line", f"\n$ {shlex.join(cmd)}\n"))
                try:
                    proc = subprocess.Popen(
                        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, bufsize=1)
                except OSError as e:
                    self._log_queue.put(("line", f"failed to start: {e}\n"))
                    self._log_queue.put(("done", False))
                    return
                self._proc = proc
                for line in iter(proc.stdout.readline, ""):
                    self._log_queue.put(("line", line))
                proc.stdout.close()
                code = proc.wait()
                self._proc = None
                if code != 0:
                    self._log_queue.put(
                        ("line", f"\n{label} FAILED (exit {code})\n"))
                    self._log_queue.put(("done", False))
                    return
                self._log_queue.put(("line", f"\n{label} OK\n"))
            self._log_queue.put(("done", True))

        threading.Thread(target=worker, daemon=True).start()

    def _append_log(self, text: str):
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _poll_log_queue(self):
        try:
            while True:
                kind, payload = self._log_queue.get_nowait()
                if kind == "line":
                    self._append_log(payload)
                elif kind == "done":
                    self._set_running(False)
                    callback, self._pending_on_success = self._pending_on_success, None
                    if payload and callback is not None:
                        callback()
        except queue.Empty:
            pass
        self.after(50, self._poll_log_queue)


def main():
    app = FlasherApp()
    app.mainloop()


if __name__ == "__main__":
    main()
