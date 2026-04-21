class LogWindow:
    def __init__(self, log_path: str, refresh_ms: int = 500, parent=None, session_start: int = 0):
        self._root = tk.Toplevel(master=parent)
        self._root.withdraw()
        self._root.title("Log Window")
        self._root.minsize(100, 80)
        self._refresh_ms = refresh_ms
        self._log_path = log_path
        import sys, os
        if getattr(sys, 'frozen', False):
            app_dir = Path(sys.executable).parent
        else:
            app_dir = Path(__file__).resolve().parents[1]
        self._window_state_path = app_dir / "log_window_state.json"
        self._session_start = session_start
        self._text = tk.Text(self._root, wrap="none", font=("Consolas", 10), state="disabled", height=20)
        self._text.pack(fill="both", expand=True)
        self._last_size = 0
        # Restore window geometry if available and valid
        try:
            if self._window_state_path.exists():
                with open(self._window_state_path, "r", encoding="utf-8") as f:
                    state = json.load(f)
                    geom = state.get("geometry", "")
                    m = re.match(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", geom or "")
                    if m:
                        width, height, x, y = map(int, m.groups())
                        if width >= 100 and height >= 100:
                            x = max(0, x)
                            y = max(0, y)
                            self._root.update_idletasks()
                            self._root.geometry(f"{width}x{height}+{x}+{y}")
        except Exception:
            pass
        self._root.deiconify()  # show window now that position is set
        self._root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._load_full_log()
        self._refresh()

    def _save_geometry(self):
        try:
            geom = self._root.geometry()
            with open(self._window_state_path, "w", encoding="utf-8") as f:
                json.dump({"geometry": geom}, f)
        except Exception:
            pass

    def _is_valid_geometry(self, geom: str) -> bool:
        # Accepts geometry like '320x290+100+100' or '320x290+0+0'
        import re
        m = re.match(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", geom)
        if not m:
            return False
        width, height, x, y = map(int, m.groups())
        # Consider negative or very large values as invalid
        if width < 100 or height < 100:
            return False
        if x < -50 or y < -50:
            return False
        # Optionally, check against screen size
        try:
            self._root.update_idletasks()
            screen_w = self._root.winfo_screenwidth()
            screen_h = self._root.winfo_screenheight()
            if x > screen_w - 50 or y > screen_h - 50:
                return False
        except Exception:
            pass
        return True
        self._root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._load_full_log()
        self._refresh()

    def _load_full_log(self):
        # Start from session_start — skip old data but show everything from this session
        self._last_size = self._session_start

    def _refresh(self):
        try:
            with open(self._log_path, "rb") as f:
                f.seek(self._last_size)
                new_bytes = f.read()
                self._last_size = f.tell()
            if new_bytes:
                lines = new_bytes.decode("utf-8", errors="replace")
                self._text.config(state="normal")
                self._text.insert("end", lines)
                self._text.see("end")
                self._text.config(state="disabled")
        except Exception:
            pass
        self._root.after(self._refresh_ms, self._refresh)

    def _on_close(self):
        self._save_geometry()
        self._root.destroy()

import json
import re
import threading
import time

_recent_printed: list = ["", ""]

def _dprint(msg: str) -> None:
    """Print only if message was not one of the last 2 printed lines."""
    global _recent_printed
    if msg not in _recent_printed:
        _recent_printed = [_recent_printed[1], msg]
        # print(msg)  # Console output disabled; uncomment to re-enable
from datetime import datetime
from pathlib import Path
import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog
from tkinter import ttk
from typing import Any

from rig_adapter import RigAdapter, load_legal_bands
from serial_transport import SerialTransport, omnirig_port_details, invalidate_omnirig_cache, omnirig_ini_mtime
from version import __version__, DEBUG_MODE

# Baud rate for the FTDI232 auxiliary serial port on the Arduino.
# Must match FTDI_BAUD in main.cpp.
_KNOB_BAUD = 57600

def _radio_button(
    parent: tk.Widget,
    text: str,
    variable: tk.IntVar,
    value: int,
    command: Any,
    font: tuple = ("Segoe UI", 9),
    fg: str = "#000000",
    gap: int = 2,
) -> tk.Canvas:
    """Custom radio button: circle indicator immediately next to text, no tkinter gap."""
    font_obj = tkfont.Font(font=font)
    text_w = font_obj.measure(text)
    text_h = font_obj.metrics("linespace")
    r = text_h // 2 - 1          # circle radius
    dot_r = r - 3                 # inner filled dot radius
    circle_w = r * 2 + 2
    width = circle_w + gap + text_w + 2
    height = text_h + 2
    parent_bg = _widget_bg(parent)

    canvas = tk.Canvas(parent, width=width, height=height,
                       bg=parent_bg, highlightthickness=0, bd=0, cursor="hand2")

    cx = r + 1
    cy = height // 2

    def _draw():
        canvas.delete("all")
        selected = variable.get() == value
        canvas.create_oval(cx - r, cy - r, cx + r, cy + r,
                           outline="#555555", fill="white", width=1)
        if selected:
            canvas.create_oval(cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r,
                               fill="#333333", outline="")
        canvas.create_text(circle_w + gap, cy, text=text, font=font,
                           fill=fg, anchor="w")

    _draw()
    variable.trace_add("write", lambda *_: _draw())

    def _click(_):
        variable.set(value)
        command()

    canvas.bind("<Button-1>", _click)
    return canvas


def _rounded_button(
    parent: tk.Widget,
    text: str,
    command: Any,
    font: tuple[str, int, str] = ("Segoe UI", 10, "bold"),
    bg: str = "#cfe8ff",
    active_bg: str = "#b8dcff",
    fg: str = "#0d2a45",
    radius: int = 12,
    padx: int = 12,
    pady: int = 6,
) -> tk.Canvas:
    font_obj = tkfont.Font(font=font)
    text_w = font_obj.measure(text)
    text_h = font_obj.metrics("linespace")
    width = text_w + (padx * 2)
    height = text_h + (pady * 2)
    corner = max(2, min(radius, width // 2, height // 2))
    parent_bg = _widget_bg(parent)

    canvas = tk.Canvas(parent, width=width, height=height + 2, bg=parent_bg, highlightthickness=0, bd=0, cursor="hand2")

    def _draw_round(x: int, y: int, w: int, h: int, r: int, fill: str, outline: str) -> None:
        points = [
            x + r, y,
            x + w - r, y,
            x + w, y,
            x + w, y + r,
            x + w, y + h - r,
            x + w, y + h,
            x + w - r, y + h,
            x + r, y + h,
            x, y + h,
            x, y + h - r,
            x, y + r,
            x, y,
        ]
        canvas.create_polygon(points, smooth=True, splinesteps=16, fill=fill, outline=outline)

    def _paint(fill_color: str, pressed: bool = False) -> None:
        canvas.delete("all")
        y_offset = 1 if pressed else 0
        text_offset = 1 if pressed else 0
        shadow_color = "#7da6cf"
        border_color = "#6f98c2"

        if not pressed:
            _draw_round(0, 2, width, height - 2, corner, shadow_color, shadow_color)

        _draw_round(0, y_offset, width, height - 2, corner, fill_color, border_color)

        if not pressed:
            # top highlight for raised effect
            canvas.create_line(corner + 2, 2, width - corner - 2, 2, fill="#eef7ff", width=1)

        canvas.create_text(width // 2, (height - 2) // 2 + text_offset, text=text, font=font, fill=fg)

    _paint(bg, pressed=False)

    def _press(_: Any) -> None:
        _paint(active_bg, pressed=True)

    def _release(event: Any) -> None:
        inside = 0 <= event.x <= width and 0 <= event.y <= height
        _paint(bg, pressed=False)
        if inside:
            command()

    def _enter(_: Any) -> None:
        _paint(active_bg, pressed=False)

    def _leave(_: Any) -> None:
        _paint(bg, pressed=False)

    canvas.bind("<ButtonPress-1>", _press)
    canvas.bind("<ButtonRelease-1>", _release)
    canvas.bind("<Enter>", _enter)
    canvas.bind("<Leave>", _leave)
    return canvas


def _fmt_hz(hz: int) -> str:
    grouped_hz = f"{hz:,}".replace(",", ".")
    return f"{grouped_hz} Hz"


def _widget_bg(parent: tk.Widget) -> str:
    try:
        return str(parent.cget("bg"))
    except tk.TclError:
        pass

    try:
        style = ttk.Style(parent)
        for style_name in ("TFrame", "TLabel", "."):
            color = style.lookup(style_name, "background")
            if color:
                return str(color)
    except Exception:
        pass

    try:
        return str(parent.winfo_toplevel().cget("bg"))
    except Exception:
        return "#f0f0f0"


class RigMonitorWindow:
    def __init__(self, rig: RigAdapter, refresh_ms: int = 200, log_path: str | None = None) -> None:

        # --- Create the root window first ---
        self._root = tk.Tk()
        self._root.title(f"Smart Knob v{__version__}")
        self._root.attributes("-topmost", True)  # Always on top
        self._root.resizable(True, True)
        self._root.minsize(100, 80)

        # --- Now initialize all instance variables that depend on Tk ---
        self._rig = rig
        self._log_path = log_path
        self._log_last_values: tuple | None = None
        self._log_compare_keys = ['freq_a', 'freq_b', 'split', 'vfo_route']
        self._log_counter = 0
        # Record file size before any writes this session, so the log window skips old data
        self._log_session_start = 0
        if log_path:
            try:
                with open(log_path, 'rb') as f:
                    f.seek(0, 2)
                    self._log_session_start = f.tell()
            except Exception:
                self._log_session_start = 0
        # Write session separator so each run is clearly marked in the log file
        if log_path:
            try:
                with open(log_path, 'a', encoding='utf-8') as f:
                    f.write(f"\n=== SESSION START {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} ===\n")
            except Exception:
                pass
        self._refresh_ms = max(100, refresh_ms)
        self._pending_freq_hz: int | None = None
        self._pending_freq_vfo: str | None = None
        self._knob_discard_until: float = 0.0
        self._legal_bands = load_legal_bands()

        self._freq_display_gate_until: float = 0.0
        self._set_freq_gen: int = 0
        self._radio_type_var = tk.StringVar(value="-")
        self._profile_file_var = tk.StringVar(value="-")
        self._loaded_models_var = tk.StringVar(value="-")
        self._vfo_a_name_var = tk.StringVar(value="VFO A")
        self._vfo_b_name_var = tk.StringVar(value="VFO B")
        self._vfo_a_var = tk.StringVar(value="-")
        self._vfo_b_var = tk.StringVar(value="-")
        self._current_freq_var = tk.StringVar(value="-")
        self._vfo_route_var = tk.StringVar(value="-")
        self._knob_target_var = tk.StringVar(value="-")
        self._split_var = tk.StringVar(value="-")
        self._omnirig_report_var = tk.StringVar(value="OmniRig: Checking...")
        self._knob_report_var = tk.StringVar(value="Knob: Not connected")
        self._radio_port_details = omnirig_port_details()
        self._last_port_check: float = 0.0
        self._omnirig_ini_mtime: float = omnirig_ini_mtime()
        self._port_main_row: tk.Frame | None = None
        self._port_rig_sel_row: tk.Frame | None = None
        self._port_status_row: tk.Frame | None = None
        self._dyn_port_widgets: list[tk.Widget] = []   # port labels in main_row
        self._dyn_rig_labels:  list[tk.Widget] = []    # "Rig 1/2" labels in rig_sel_row
        self._dyn_separators:  list[tk.Widget] = []    # red lines in status_row
        self._debug_var = tk.StringVar(value="Debug: -")
        self._radio_type_name_label: tk.Label | None = None
        self._radio_type_value_label: tk.Label | None = None
        self._vfo_a_name_label: tk.Label | None = None
        self._vfo_a_value_label: tk.Label | None = None
        self._vfo_a_name_container: tk.Frame | None = None
        self._vfo_b_name_label: tk.Label | None = None
        self._vfo_b_value_label: tk.Label | None = None
        self._vfo_b_name_container: tk.Frame | None = None
        self._omnirig_report_label: tk.Label | None = None
        self._knob_report_label: tk.Label | None = None
        self._knob_status_until: float = 0.0
        self._last_knob_probe: float = 0.0
        self._last_knob_port: str | None = None
        self._probe_running: bool = False
        self._transport: SerialTransport | None = None
        self._band_table_sent: bool = False
        self._last_freq_send: float = 0.0
        self._freq_send_interval: float = 1.0
        self._last_freq_cmd_time: float = 0.0
        self._freq_flush_pending: bool = False
        self._last_omnirig_success_time: float = time.monotonic()
        self._freq_fail_count: int = 0
        self._freq_fail_threshold: int = 10
        self._omnirig_fail_count: int = 0
        self._omnirig_fail_threshold: int = 10
        self._last_omnirig_report: str = ""
        self._row_default_bg: str | None = None
        self._calib_instruction_var = tk.StringVar(
            value="Calibration Wizard: press Start Calibration to begin guided 4-state testing."
        )
        self._calib_progress_var = tk.StringVar(value="Calibration: idle")
        self._calib_phase = "idle"
        self._calib_index = -1
        self._calib_current: dict[str, Any] | None = None
        self._calib_records: list[dict[str, Any]] = []
        self._calib_scenarios: list[dict[str, str]] = [
            {
                "id": "off_knob_a",
                "title": "1/4 Split OFF, knob on A",
                "setup": "Set split OFF and make the real radio knob control VFO A.",
            },
            {
                "id": "off_knob_b",
                "title": "2/4 Split OFF, knob on B",
                "setup": "Set split OFF and make the real radio knob control VFO B.",
            },
            {
                "id": "on_start_a",
                "title": "3/4 Split ON, start from A",
                "setup": "Start from knob on A, then enable split.",
            },
            {
                "id": "on_start_b",
                "title": "4/4 Split ON, start from B",
                "setup": "Start from knob on B, then enable split.",
            },
        ]

        # Restore window geometry if available
        import sys, os
        if getattr(sys, 'frozen', False):
            app_dir = Path(sys.executable).parent
        else:
            app_dir = Path(__file__).resolve().parents[1]
        self._window_state_path = app_dir / "main_window_state.json"
        self._omnirig_rig_var = tk.IntVar(value=1)
        try:
            if self._window_state_path.exists():
                with open(self._window_state_path, "r", encoding="utf-8") as f:
                    state = json.load(f)
                    geom = state.get("geometry", "")
                    m = re.match(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", geom or "")
                    if m:
                        width, height, x, y = map(int, m.groups())
                        self._root.update_idletasks()
                        sw = self._root.winfo_screenwidth()
                        sh = self._root.winfo_screenheight()
                        # Clamp to visible area
                        x = max(0, min(x, sw - 100))
                        y = max(0, min(y, sh - 80))
                        self._root.geometry(f"{width}x{height}+{x}+{y}")
                    saved_rig = state.get("omnirig_rig", 1)
                    self._omnirig_rig_var.set(saved_rig)
                    self._rig.set_rig_number(saved_rig)
        except Exception:
            pass

        # Save geometry on close
        self._root.protocol("WM_DELETE_WINDOW", self._on_close)

        # Build the GUI layout
        self._build_layout()
    def _on_close(self):
        # Save log window position before any destruction
        lw = getattr(self, "_log_window", None)
        if lw is not None:
            lw._save_geometry()
        try:
            geom = self._root.geometry()
            with open(self._window_state_path, "w", encoding="utf-8") as f:
                json.dump({"geometry": geom, "omnirig_rig": self._omnirig_rig_var.get()}, f)
        except Exception:
            pass
        self._root.quit()
        self._root.destroy()

    def _build_layout(self) -> None:
        self._root.columnconfigure(0, weight=1)
        # Place the main frame below the custom title bar
        frame = ttk.Frame(self._root, padding=6)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        frame.columnconfigure(2, weight=0)
        frame.columnconfigure(3, weight=0)

        # Remove header label since title is in the custom title bar
        # Pull up all other lines for a more compact layout
        frame.grid_columnconfigure(0, weight=0)
        frame.grid_columnconfigure(1, weight=0)
        frame.grid_columnconfigure(2, weight=0)
        frame.grid_columnconfigure(3, weight=1)

        self._radio_type_name_label, self._radio_type_value_label = self._row(frame, 1, "Radio Type", self._radio_type_var)
        self._vfo_a_name_container = tk.Frame(frame)
        self._vfo_a_name_container.grid(row=2, column=0, sticky="w", pady=0)
        self._vfo_a_value_label = tk.Label(frame, textvariable=self._vfo_a_var, font=("Consolas", 12), anchor="w")
        self._vfo_a_value_label.grid(row=2, column=1, sticky="w", pady=0)
        self._render_vfo_a_name_label()
        self._vfo_b_name_container = tk.Frame(frame)
        self._vfo_b_name_container.grid(row=3, column=0, sticky="w", pady=0)
        self._vfo_b_value_label = tk.Label(frame, textvariable=self._vfo_b_var, font=("Consolas", 12), anchor="w")
        self._vfo_b_value_label.grid(row=3, column=1, sticky="w", pady=0)
        self._render_vfo_b_name_label()
        # Place Split Mode and VFO Route on the same row to save space
        # Place Split Mode value immediately after the label
        # Place Split Mode (bold) and value, then VFO Route and value, all close together on the same row
        # Render Split Mode and value as a single label, left-justified, with no space between
        # Combine Split Mode and VFO Route into a single label for perfect left alignment
        # Use a Text widget to color YES/NO
        split_vfo_text = tk.Text(frame, height=1, width=48, borderwidth=0, highlightthickness=0, font=("Segoe UI", 11, "bold"))
        split_vfo_text.grid(row=4, column=0, sticky="w", padx=0, pady=0, columnspan=4)
        split_vfo_text.tag_configure("yes", foreground="#0b5f0b")
        split_vfo_text.tag_configure("no", foreground="blue")
        split_vfo_text.tag_configure("label", font=("Segoe UI", 11, "bold"))
        split_vfo_text.tag_configure("route", font=("Segoe UI", 11, "bold"))
        split_vfo_text.config(state="disabled")

        def update_split_vfo_text(*args):
            split_vfo_text.config(state="normal")
            split_vfo_text.delete("1.0", "end")
            split_val = self._split_var.get().strip().upper()
            color_tag = "yes" if split_val == "YES" else "no"
            split_vfo_text.insert("end", "Split Mode: ", "label")
            split_vfo_text.insert("end", self._split_var.get(), color_tag)
            split_vfo_text.insert("end", "        VFO Route: ", "route")
            split_vfo_text.insert("end", self._vfo_route_var.get())
            split_vfo_text.config(state="disabled")
        self._split_var.trace_add('write', update_split_vfo_text)
        self._vfo_route_var.trace_add('write', update_split_vfo_text)
        update_split_vfo_text()
        # Force RX/TX frequency refresh after split mode changes
        def force_freq_refresh(*args):
            if time.monotonic() < self._freq_display_gate_until:
                return
            if self._last_omnirig_report == "Not active":
                self._vfo_a_var.set("---------")
                self._vfo_b_var.set("---------")
                return
            try:
                freq_a = self._rig.get_raw_param("FreqA")
                freq_b = self._rig.get_raw_param("FreqB")
                self._vfo_a_var.set(_fmt_hz(freq_a) if freq_a else "---")
                self._vfo_b_var.set(_fmt_hz(freq_b) if freq_b else "---")
            except Exception:
                self._vfo_a_var.set("---------")
                self._vfo_b_var.set("---------")
        self._split_var.trace_add('write', force_freq_refresh)
        # self._row(frame, 6, "Current Knob Freq", self._current_freq_var, pady=0)  # Commented out for later use if needed

        if self._radio_type_name_label is not None:
            self._radio_type_name_label.configure(font=("Segoe UI", 12, "bold"))
        if self._radio_type_value_label is not None:
            self._radio_type_value_label.configure(font=("Consolas", 13, "bold"))


        # Add horizontal separator (dark line) above OmniRig and Knob report lines
        separator = ttk.Separator(frame, orient="horizontal")
        separator.grid(row=7, column=0, columnspan=4, sticky="ew", pady=(2, 0))

        status_row = tk.Frame(frame)
        status_row.grid(row=8, column=0, columnspan=4, sticky="ew")

        # RIG selector row — also hosts the floating "Rig 1"/"Rig 2" port-group labels
        rig_sel_row = tk.Frame(status_row)
        rig_sel_row.pack(side=tk.TOP, fill=tk.X)

        def _on_rig_changed():
            n = self._omnirig_rig_var.get()
            self._rig.set_rig_number(n)
            try:
                geom = self._root.geometry()
                with open(self._window_state_path, "w", encoding="utf-8") as f:
                    json.dump({"geometry": geom, "omnirig_rig": n}, f)
            except Exception:
                pass

        tk.Label(rig_sel_row, text="Connect:", font=("Segoe UI", 9), padx=0, pady=0, bd=0).pack(
            side=tk.LEFT)
        for val, txt in ((1, "RIG1"), (2, "RIG2")):
            _radio_button(
                rig_sel_row, txt, self._omnirig_rig_var, val, _on_rig_changed,
            ).pack(side=tk.LEFT, padx=(6, 0) if txt == "RIG1" else (2, 0))

        # Main row: OmniRig status + port labels
        main_row = tk.Frame(status_row)
        main_row.pack(side=tk.TOP, fill=tk.X)
        self._port_main_row    = main_row
        self._port_rig_sel_row = rig_sel_row
        self._port_status_row  = status_row

        self._omnirig_report_label = tk.Label(
            main_row,
            textvariable=self._omnirig_report_var,
            fg="#b00020",
            font=("Segoe UI", 11, "bold"),
            anchor="w", padx=0, pady=0, bd=0,
        )
        self._omnirig_report_label.pack(side=tk.LEFT)

        tk.Label(main_row, text="", padx=0, pady=0, bd=0).pack(
                     side=tk.LEFT, padx=(12, 2))

        self._build_port_labels()
        frame.after(200, self._place_rig_labels)

        self._knob_report_label = tk.Label(
            frame,
            textvariable=self._knob_report_var,
            fg="#b00020",
            font=("Segoe UI", 11, "bold"),
            anchor="w",
        )
        self._knob_report_label.grid(row=9, column=0, columnspan=4, sticky="ew", pady=(0, 0))

        # --- R&D DEBUG BAR: remove this block when research is done ---
        self._debug_label = tk.Label(
            frame,
            textvariable=self._debug_var,
            fg="#555555",
            font=("Consolas", 9),
            anchor="w",
            wraplength=500,
            justify="left",
            cursor="hand2",
        )
        if DEBUG_MODE:
            self._debug_label.grid(row=10, column=0, columnspan=4, sticky="ew", pady=(4, 0))
        self._debug_label.bind("<Button-1>", lambda e: self._copy_debug_to_clipboard())
        # --- END R&D DEBUG BAR ---

        # The following lines are hidden from the UI but remain in the code for future use:
        # self._row(frame, 10, "Profile File", self._profile_file_var, pady=0)
        # profile_button = ttk.Button(frame, text="Browse Profile INI", command=self._browse_profile_file)
        # profile_button.grid(row=11, column=0, sticky="w", pady=(4, 0))

        if self._vfo_b_name_container is not None:
            self._row_default_bg = self._vfo_b_name_container.cget("bg")

    def _set_na_values(self) -> None:
        self._freq_fail_count = 0  # Reset fail counter when N/A is set
        self._radio_type_var.set("N/A")
        self._vfo_a_var.set("---------")
        self._vfo_b_var.set("---------")
        self._current_freq_var.set("N/A")
        self._vfo_route_var.set("N/A")
        self._knob_target_var.set("N/A")
        self._split_var.set("N/A")

    def _refresh_row_labels(self, split: bool | None = None) -> None:
        row_a_label, row_b_label = self._rig.get_row_labels()
        if self._rig.uses_display_slot_mode():
            # IC-7300 style: OmniRig always puts knob freq in slot A regardless of A/B.
            if split is True:
                row_a_label = "Knob Freq (RX)"
                row_b_label = "Sub Freq (TX)"
            elif split is False:
                row_a_label = "Knob Freq (RXTX)"
                row_b_label = "Sub Freq"
        else:
            # Generic radio: labels are fixed (Knob Freq top, Sub Freq bottom).
            # Hz values are swapped in _refresh when the knob is on VFO B.
            if split is True:
                knob_role = self._rig.get_split_knob_role()
                if knob_role == "tx":
                    row_a_label = "Knob Freq (TX)"
                    row_b_label = "Sub Freq (RX)"
                elif knob_role == "rx":
                    row_a_label = "Knob Freq (RX)"
                    row_b_label = "Sub Freq (TX)"
                else:
                    knob_vfo = self._rig.get_knob_display_vfo()
                    route = self._rig.get_vfo_route()  # e.g. "AB": route[0]=RX, route[1]=TX
                    if route and len(route) == 2 and knob_vfo == route[1]:
                        row_a_label = "Knob Freq (TX)"
                        row_b_label = "Sub Freq (RX)"
                    else:
                        row_a_label = "Knob Freq (RX)"
                        row_b_label = "Sub Freq (TX)"
            elif split is False:
                row_a_label = "Knob Freq (RXTX)"
                row_b_label = "Sub Freq"
            else:
                row_a_label = "Knob Freq"
                row_b_label = "Sub Freq"
        self._vfo_a_name_var.set(row_a_label)
        self._vfo_b_name_var.set(row_b_label)

        self._render_vfo_a_name_label()
        self._render_vfo_b_name_label()

    def _render_vfo_a_name_label(self) -> None:
        if self._vfo_a_name_container is None:
            return

        for child in self._vfo_a_name_container.winfo_children():
            child.destroy()

        text = self._vfo_a_name_var.get()
        base_font = ("Segoe UI", 11, "bold")
        match = re.fullmatch(r"Knob Freq \((RX)(\s*/\s*)(TX)\)", text)
        if match:
            tk.Label(self._vfo_a_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="/", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="TX", font=base_font, fg="#d00000", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_a_name_label = tk.Label(self._vfo_a_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_a_name_label.pack(side="left", padx=0, pady=0)
            return

        # Add support for Knob Freq (RXTX) label with colored RX and TX
        match = re.fullmatch(r"Knob Freq \((RXTX)\)", text)
        if match:
            tk.Label(self._vfo_a_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="TX", font=base_font, fg="#d00000", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_a_name_label = tk.Label(self._vfo_a_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_a_name_label.pack(side="left", padx=0, pady=0)
            return

        match = re.fullmatch(r"Knob Freq \((RX)\)", text)
        if match:
            tk.Label(self._vfo_a_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_a_name_label = tk.Label(self._vfo_a_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_a_name_label.pack(side="left", padx=0, pady=0)
            return

        match = re.fullmatch(r"Knob Freq \((TX)\)", text)
        if match:
            tk.Label(self._vfo_a_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_a_name_container, text="TX", font=base_font, fg="#d00000", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_a_name_label = tk.Label(self._vfo_a_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_a_name_label.pack(side="left", padx=0, pady=0)
            return

        self._vfo_a_name_label = tk.Label(self._vfo_a_name_container, text=text + ":", font=base_font, anchor="w", padx=0)
        self._vfo_a_name_label.pack(side="left", padx=0, pady=0)

    def _render_vfo_b_name_label(self) -> None:
        if self._vfo_b_name_container is None:
            return

        for child in self._vfo_b_name_container.winfo_children():
            child.destroy()

        text = self._vfo_b_name_var.get()
        base_font = ("Segoe UI", 11, "bold")
        match = re.fullmatch(r"Knob Freq \((RX)(\s*/\s*)(TX)\)", text)
        if match:
            tk.Label(self._vfo_b_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="/", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="TX", font=base_font, fg="#d00000", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_b_name_label = tk.Label(self._vfo_b_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_b_name_label.pack(side="left", padx=0, pady=0)
            return

        # Add support for Knob Freq (RXTX) label with colored RX and TX
        match = re.fullmatch(r"Knob Freq \((RXTX)\)", text)
        if match:
            tk.Label(self._vfo_b_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="TX", font=base_font, fg="#d00000", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_b_name_label = tk.Label(self._vfo_b_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_b_name_label.pack(side="left", padx=0, pady=0)
            return

        match = re.fullmatch(r"Knob Freq \((RX)\)", text)
        if match:
            tk.Label(self._vfo_b_name_container, text="Knob Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_b_name_label = tk.Label(self._vfo_b_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_b_name_label.pack(side="left", padx=0, pady=0)
            return

        match = re.fullmatch(r"Sub Freq \((TX)\)", text)
        if match:
            tk.Label(self._vfo_b_name_container, text="Sub Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="TX", font=base_font, fg="#d00000", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_b_name_label = tk.Label(self._vfo_b_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_b_name_label.pack(side="left", padx=0, pady=0)
            return

        match = re.fullmatch(r"Sub Freq \((RX)\)", text)
        if match:
            tk.Label(self._vfo_b_name_container, text="Sub Freq (", font=base_font, anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            tk.Label(self._vfo_b_name_container, text="RX", font=base_font, fg="#0b5f0b", anchor="w", padx=0).pack(side="left", padx=0, pady=0)
            self._vfo_b_name_label = tk.Label(self._vfo_b_name_container, text=")", font=base_font, anchor="w", padx=0)
            self._vfo_b_name_label.pack(side="left", padx=0, pady=0)
            return

        self._vfo_b_name_label = tk.Label(self._vfo_b_name_container, text=text + ":", font=base_font, anchor="w", padx=0)
        self._vfo_b_name_label.pack(side="left", padx=0, pady=0)

    def _apply_split_visual_effect(self, split: bool | None) -> None:
        if self._row_default_bg is None:
            return

        a_fg = "#111111"
        b_fg = "#111111"
        b_bg = self._row_default_bg

        if split is False:
            b_fg = "#8a8a8a"

        if self._vfo_a_name_label is not None:
            self._vfo_a_name_label.configure(fg=a_fg, bg=self._row_default_bg)
        if self._vfo_a_value_label is not None:
            self._vfo_a_value_label.configure(fg=a_fg, bg=self._row_default_bg)
        if self._vfo_b_name_label is not None:
            self._vfo_b_name_label.configure(fg=b_fg, bg=b_bg)
        if self._vfo_b_value_label is not None:
            self._vfo_b_value_label.configure(fg=b_fg, bg=b_bg)

    def _refresh_profile_file_label(self) -> None:
        self._profile_file_var.set(Path(self._rig.get_profile_ini_path()).name)

    def _refresh_loaded_models_label(self) -> None:
        models = self._rig.get_supported_radio_models()
        self._loaded_models_var.set(", ".join(models) if models else "(none listed in profile file)")

    def _log_state(self, freq_a: int, freq_b: int, split, vfo_route) -> None:
        if self._log_path is None:
            return
        state = {
            'freq_a': freq_a,
            'freq_b': freq_b,
            'split': split,
            'vfo_route': vfo_route,
            'tx_vfo': self._rig.get_tx_vfo(),
        }
        current_values = tuple(state.get(k, '-') for k in self._log_compare_keys)
        if self._log_last_values == current_values:
            return
        self._log_last_values = current_values
        self._log_counter += 1
        fields = [
            f"freq_a={freq_a}",
            f"freq_b={freq_b}",
            f"split={split}",
            f"vfo_route={vfo_route}",
            f"tx_vfo={state['tx_vfo']}",
            f"raw_vfo={self._rig.get_raw_param('Vfo')}",
            f"active_vfo={self._rig.get_active_vfo()}",
            f"knob_vfo={self._rig.get_knob_display_vfo()}",
            f"radio_knob_vfo={self._rig.get_radio_knob_vfo()}",
        ]
        line = f"{self._log_counter}) " + ', '.join(fields)
        try:
            with open(self._log_path, 'a', encoding='utf-8') as f:
                f.write(line + '\n')
        except Exception:
            pass

    def _browse_profile_file(self) -> None:
        current = Path(self._rig.get_profile_ini_path())
        initial_dir = str(current.parent) if current.parent.exists() else str(Path(__file__).resolve().parents[1])
        selected = filedialog.askopenfilename(
            title="Select radio profile INI",
            initialdir=initial_dir,
            filetypes=[("INI files", "*.ini"), ("All files", "*.*")],
        )
        if not selected:
            return
        try:
            models = self._rig.set_profile_ini_path(selected)
            self._refresh_profile_file_label()
            self._refresh_loaded_models_label()
            self._refresh_row_labels()
            if models:
                self._set_knob_report(f"Loaded profile file ({len(models)} models)", ok=True)
            else:
                self._set_knob_report("Profile file loaded (no supported models listed)", ok=False)
        except Exception as exc:
            self._set_knob_report(f"Profile load error ({exc})", ok=False)
        self._knob_status_until = time.monotonic() + 3.0

    def _set_omnirig_report(self, text: str, ok: bool) -> None:
        self._omnirig_report_var.set(f"OmniRig: {text}")
        if self._omnirig_report_label is not None:
            self._omnirig_report_label.configure(fg="#0a7a32" if ok else "#b00020")

    def _set_knob_report(self, text: str, ok: bool) -> None:
        # Always show only 'Active on COM#' or 'Not Active' (with port if known)
        if ok and self._last_knob_port:
            status = f"Active on {self._last_knob_port}"
        elif self._last_knob_port:
            status = f"Not Active (last: {self._last_knob_port})"
        else:
            status = "Not Active"
        self._knob_report_var.set(f"Knob: {status}")
        if self._knob_report_label is not None:
            self._knob_report_label.configure(fg="#0a7a32" if ok else "#b00020")

    def _detect_knob_port_async(self) -> None:
        if self._probe_running:
            return
        self._probe_running = True

        def worker():
            probe_errors: list[str] = []
            found_port: str | None = None
            for port in SerialTransport.candidate_ports():
                print(f"[probe] trying {port}")
                matched, detail = SerialTransport.probe_device_identity_details(port, baudrate=_KNOB_BAUD)
                if matched:
                    found_port = port
                    break
                if detail:
                    probe_errors.append(f"{port}: {detail}")
            if found_port is not None:
                self._root.after(0, lambda p=found_port: self._on_knob_port_detected(p, None))
            elif probe_errors:
                self._root.after(0, lambda e=probe_errors[0]: self._on_knob_port_detected(None, e))
            else:
                self._root.after(0, lambda: self._on_knob_port_detected(None, None))

        threading.Thread(target=worker, daemon=True).start()

    def _open_knob_transport(self, port: str) -> None:
        self._close_knob_transport()
        try:
            t = SerialTransport()
            t.open(port, baudrate=_KNOB_BAUD)
            self._transport = t
            self._last_freq_send = time.monotonic()
            self._start_knob_reader()
        except Exception:
            self._transport = None

    def _send_band_table_to_arduino(self) -> None:
        """Send the full legal band table to the Arduino once on connect/reconnect."""
        if self._transport is None or not self._transport.is_connected:
            return
        if self._band_table_sent:

            return
        self._band_table_sent = True
        try:
            self._transport.write_line("")  # flush any partial bytes left by probe
            time.sleep(0.05)
            print(f"[BANDS] sending {len(self._legal_bands)} bands")
            for i, (low, high, _) in enumerate(self._legal_bands):
                msg = f"BAND_ADD:{low}:{high}"
                cs = 0
                for c in msg:
                    cs ^= ord(c)
                self._transport.write_line(f"{msg}*{cs:02X}")
                print(f"[BANDS] sent BAND_ADD {i}: {low}:{high}")
                time.sleep(0.1)
            self._transport.write_line(f"BAND_DONE:{len(self._legal_bands)}")
            print(f"[BANDS] sent BAND_DONE:{len(self._legal_bands)}")
        except Exception as e:
            print(f"[BANDS] ERROR: {e}")


    def _start_knob_reader(self) -> None:
        """Background thread that reads encoder events from the Arduino."""
        def reader() -> None:
            while True:
                t = self._transport
                if t is None or not t.is_connected:
                    break
                try:
                    line = t.read_line()
                except Exception:
                    self._root.after(0, self._handle_serial_disconnect)
                    break
                if line is None:
                    continue
                if not line.startswith("HELLO_ARDUINO"):
                    _dprint(f"[reader] received: {repr(line)}")
                if line.startswith("HELLO_ARDUINO"):
                    self._send_band_table_to_arduino()
                elif line.startswith("SET_FREQ:"):
                    payload = line[9:]
                    self._root.after(0, lambda p=payload: self._on_set_freq(p))
                elif line.startswith("SNAP_FREQ:"):
                    payload = line[10:]
                    _dprint(f"[reader] dispatching SNAP_FREQ: {payload}")
                    self._root.after(0, lambda p=payload: self._on_snap_freq(p))
                elif line == "NO_BASE_FREQ":
                    _dprint(f"[reader] NO_BASE_FREQ — Arduino has no base frequency yet")
                    self._root.after(0, self._on_no_base_freq)
                elif line == "SET_SPLIT:ON":
                    self._root.after(0, lambda: self._on_set_split(True))
                elif line == "SET_SPLIT:OFF":
                    self._root.after(0, lambda: self._on_set_split(False))
                elif line.startswith("BTN:"):
                    try:
                        n = int(line[4:])
                        self._root.after(0, lambda n=n: self._on_btn_press(n))
                    except ValueError:
                        pass
                elif line.startswith("DBG:"):
                    print(f"[ARDUINO] {line}")
                elif line.startswith("BANDS:") or line.startswith("BAND:"):
                    print(f"[BANDS] {line}")


        threading.Thread(target=reader, daemon=True).start()

    def _close_knob_transport(self) -> None:
        if self._transport is not None:
            try:
                self._transport.close()
            except Exception:
                pass
            self._transport = None
        self._band_table_sent = False

    def _handle_serial_disconnect(self) -> None:
        """Called from the reader thread when a serial error forces it to exit."""
        self._close_knob_transport()
        self._last_knob_port = None
        self._set_knob_report("Knob: disconnected", ok=False)

    def _on_knob_port_detected(self, port: str | None, error: str | None) -> None:
        self._probe_running = False
        if port:
            self._last_knob_port = port
            self._set_knob_report(f"Knob detected on {port}", ok=True)
            self._open_knob_transport(port)
        elif error:
            self._last_knob_port = None
            self._close_knob_transport()
            self._set_knob_report(f"Knob detection error: {error}", ok=False)
        else:
            self._last_knob_port = None
            self._close_knob_transport()
            self._set_knob_report("Knob not found", ok=False)

    def _refresh_knob_connection_status(self) -> None:
        # Always update the knob status label to reflect the current connection state
        if self._transport is not None and self._transport.is_connected:
            self._set_knob_report("Knob: Connected", ok=True)
        else:
            self._set_knob_report("Knob: Not connected", ok=False)

        if time.monotonic() < self._knob_status_until:
            return

        now = time.monotonic()
        if now - self._last_knob_probe < 1.0:
            return
        self._last_knob_probe = now

        # If a port was already confirmed, just verify it is still listed —
        # no need to open the port again (which resets the Arduino via DTR).
        if self._last_knob_port is not None:
            from serial.tools import list_ports as _lp
            if any(i.device.upper() == self._last_knob_port.upper() for i in _lp.comports()):
                return  # Still present; keep showing "connected"
            # Port disappeared — clear state and fall through to a full probe.
            self._last_knob_port = None
            self._close_knob_transport()
            self._set_knob_report("Knob disconnected", ok=False)

        self._detect_knob_port_async()

    @staticmethod
    def _slugify(value: str) -> str:
        cleaned = [ch.lower() if ch.isalnum() else "_" for ch in value]
        slug = "".join(cleaned).strip("_")
        return slug or "radio"

    def _calibration_step_text(self) -> str:
        if self._calib_index < 0 or self._calib_index >= len(self._calib_scenarios):
            return "Calibration finished."
        scenario = self._calib_scenarios[self._calib_index]
        if self._calib_phase == "await_state":
            return f"{scenario['title']}: {scenario['setup']} Then press Capture State."
        if self._calib_phase == "await_real":
            return (
                f"{scenario['title']}: turn the REAL radio knob by one step, then press Mark: Changed A or Mark: Changed B."
            )
        if self._calib_phase == "await_software":
            return (
                f"{scenario['title']}: click Software Knob +1 kHz once, then press Mark: Changed A or Mark: Changed B."
            )
        return "Calibration idle. Press Start Calibration."

    def _capture_live_snapshot(self) -> dict[str, Any]:
        split = self._rig.read_split_mode()
        route = self._rig.get_vfo_route()
        active = self._rig.get_active_vfo()
        knob_display = self._rig.get_knob_display_vfo()
        knob_command = self._rig.get_knob_command_vfo()
        freq_a = self._rig.read_frequency("A")
        freq_b = self._rig.read_frequency("B")
        return {
            "time": datetime.now().isoformat(timespec="seconds"),
            "radio_type": self._rig.get_radio_type(),
            "split": split,
            "route": route,
            "active_vfo": active,
            "knob_display_vfo": knob_display,
            "knob_command_vfo": knob_command,
            "freq_a": freq_a,
            "freq_b": freq_b,
        }

    def _start_calibration(self) -> None:
        self._calib_records = []
        self._calib_current = None
        self._calib_index = 0
        self._calib_phase = "await_state"
        self._calib_instruction_var.set(self._calibration_step_text())
        self._calib_progress_var.set("Calibration: scenario 1/4 ready")

    def _capture_calibration_state(self) -> None:
        if self._calib_phase != "await_state" or self._calib_index < 0:
            self._calib_progress_var.set("Calibration: press Start Calibration first")
            return
        scenario = self._calib_scenarios[self._calib_index]
        self._calib_current = {
            "scenario_id": scenario["id"],
            "scenario_title": scenario["title"],
            "baseline": self._capture_live_snapshot(),
            "real_knob_changed": None,
            "software_knob_changed": None,
        }
        self._calib_phase = "await_real"
        self._calib_instruction_var.set(self._calibration_step_text())
        self._calib_progress_var.set(
            f"Calibration: captured baseline for {scenario['title']}"
        )

    def _save_calibration_profile(self) -> Path:
        project_root = Path(__file__).resolve().parents[1]
        out_dir = project_root / "calibration_profiles"
        out_dir.mkdir(parents=True, exist_ok=True)

        radio_type = self._rig.get_radio_type() or "radio"
        radio_slug = self._slugify(radio_type)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        out_path = out_dir / f"{radio_slug}_{stamp}.json"

        payload = {
            "saved_at": datetime.now().isoformat(timespec="seconds"),
            "radio_type": radio_type,
            "records": self._calib_records,
        }
        out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return out_path

    def _mark_calibration_change(self, changed_vfo: str) -> None:
        if self._calib_current is None or self._calib_index < 0:
            self._calib_progress_var.set("Calibration: no active scenario")
            return
        if changed_vfo not in {"A", "B"}:
            return

        if self._calib_phase == "await_real":
            self._calib_current["real_knob_changed"] = changed_vfo
            self._calib_phase = "await_software"
            self._calib_instruction_var.set(self._calibration_step_text())
            self._calib_progress_var.set(
                f"Calibration: real knob marked as {changed_vfo} for {self._calib_current['scenario_title']}"
            )
            return

        if self._calib_phase != "await_software":
            self._calib_progress_var.set("Calibration: capture state first")
            return

        self._calib_current["software_knob_changed"] = changed_vfo
        self._calib_current["after"] = self._capture_live_snapshot()
        self._calib_records.append(self._calib_current)

        self._calib_index += 1
        self._calib_current = None

        if self._calib_index >= len(self._calib_scenarios):
            self._calib_phase = "idle"
            out_path = self._save_calibration_profile()
            self._calib_instruction_var.set(
                "Calibration complete. Start Calibration to run again for this or another radio."
            )
            self._calib_progress_var.set(
                f"Calibration saved: {out_path.name} ({len(self._calib_records)} records)"
            )
            return

        self._calib_phase = "await_state"
        self._calib_instruction_var.set(self._calibration_step_text())
        self._calib_progress_var.set(
            f"Calibration: scenario {self._calib_index + 1}/4 ready"
        )

    @staticmethod
    def _row(parent: ttk.Frame, row: int, label: str | tk.StringVar, var: tk.StringVar, pady=0) -> tuple[tk.Label, tk.Label]:
        if isinstance(label, tk.StringVar):
            name_label = tk.Label(parent, textvariable=label, font=("Segoe UI", 11, "bold"), anchor="w")
        else:
            name_label = tk.Label(parent, text=f"{label}:", font=("Segoe UI", 11, "bold"), anchor="w")
        name_label.grid(
            row=row,
            column=0,
            sticky="w",
            pady=pady,
        )
        value_label = tk.Label(parent, textvariable=var, font=("Consolas", 12), anchor="w")
        value_label.grid(
            row=row,
            column=1,
            sticky="w",
            pady=pady,
        )
        return name_label, value_label

    def _update_active_vfo_display(self, active_vfo: str | None) -> None:
        split = self._rig.read_split_mode() if hasattr(self._rig, 'read_split_mode') else False
        if split:
            # In split mode, only TX line is bold
            tx_vfo = self._rig.get_tx_vfo() if hasattr(self._rig, 'get_tx_vfo') else active_vfo
            a_active = tx_vfo == "A"
            b_active = tx_vfo == "B"
        else:
            # In non-split, both RX and TX are bold
            a_active = True
            b_active = True

        if self._vfo_a_name_label is not None:
            self._vfo_a_name_label.configure(font=("Segoe UI", 12, "bold" if a_active else "normal"))
        if self._vfo_a_value_label is not None:
            self._vfo_a_value_label.configure(font=("Consolas", 13, "bold" if a_active else "normal"))
        if self._vfo_b_name_label is not None:
            self._vfo_b_name_label.configure(font=("Segoe UI", 12, "bold" if b_active else "normal"))
        if self._vfo_b_value_label is not None:
            self._vfo_b_value_label.configure(font=("Consolas", 13, "bold" if b_active else "normal"))

    def _copy_debug_to_clipboard(self) -> None:
        self._root.clipboard_clear()
        self._root.clipboard_append(self._debug_var.get())
        self._root.update_idletasks()
        self._set_knob_report("Debug copied", ok=True)
        self._knob_status_until = time.monotonic() + 2.0


    def _on_set_freq(self, payload: str) -> None:
        """Called when the Arduino sends SET_FREQ:<hz>:<vfo>."""
        try:
            parts = payload.split(":")
            hz = int(parts[0])
            vfo = parts[1] if len(parts) > 1 else self._rig.get_knob_display_vfo()
            if hz <= 0:
                return
            # Update display immediately for every step
            if vfo == "A":
                self._vfo_a_var.set(_fmt_hz(hz))
            elif vfo == "B":
                self._vfo_b_var.set(_fmt_hz(hz))
            self._pending_freq_hz = hz
            self._pending_freq_vfo = vfo
            self._current_freq_var.set(_fmt_hz(hz))
            self._freq_display_gate_until = time.monotonic() + 0.8
            self._set_knob_report("Knob active", ok=True)
            self._knob_status_until = time.monotonic() + 2.0
            # Debounce: only send to OmniRig once rotation pauses (50 ms after last step)
            if not self._freq_flush_pending:
                self._freq_flush_pending = True
                self._root.after(50, self._flush_freq_to_radio)
        except Exception as exc:
            self._set_knob_report(f"Set freq error: {exc}", ok=False)
            self._knob_status_until = time.monotonic() + 4.0

    def _flush_freq_to_radio(self) -> None:
        self._freq_flush_pending = False
        hz = self._pending_freq_hz
        vfo = self._pending_freq_vfo
        if not hz or hz <= 0:
            return
        try:
            self._last_freq_cmd_time = time.monotonic()
            self._rig.set_frequency(hz, vfo)
        except Exception as exc:
            self._set_knob_report(f"Set freq error: {exc}", ok=False)
            self._knob_status_until = time.monotonic() + 4.0

    def _on_snap_freq(self, payload: str) -> None:
        """Called when the Arduino sends SNAP_FREQ:<hz>:<vfo> (automatic snap, no retry)."""
        try:
            parts = payload.split(":")
            hz = int(parts[0])
            vfo = parts[1] if len(parts) > 1 else self._rig.get_knob_display_vfo()
            if hz <= 0:
                return
            if vfo == "A":
                self._vfo_a_var.set(_fmt_hz(hz))
            elif vfo == "B":
                self._vfo_b_var.set(_fmt_hz(hz))
            self._current_freq_var.set(_fmt_hz(hz))
            self._freq_display_gate_until = time.monotonic() + 0.8
            _dprint(f"[_on_snap_freq] calling set_frequency({hz}, {vfo}) — no retry")
            self._rig.set_frequency(hz, vfo)
            self._set_knob_report("Knob active", ok=True)
            self._knob_status_until = time.monotonic() + 2.0
        except Exception as exc:
            self._set_knob_report(f"Set freq error: {exc}", ok=False)
            self._knob_status_until = time.monotonic() + 4.0

    def _retry_set_freq(self, hz: int, vfo: str, gen: int, freq_a_baseline: int, attempt: int = 1) -> None:
        """Retry set_frequency — OmniRig may be in 'not responding' state on first call."""
        if self._set_freq_gen != gen:
            return  # User has already sent a newer command, don't override
        status = self._rig.get_raw_param("Status")
        freq_a = self._rig.get_raw_param("FreqA")
        _dprint(f"[_retry_set_freq] omnirig_status={status} omnirig_freqA={freq_a} target={hz} baseline={freq_a_baseline} gen={gen}")
        if freq_a and freq_a_baseline and abs(freq_a - freq_a_baseline) > 1_000_000:
            _dprint(f"[_retry_set_freq] aborting: radio moved from baseline {freq_a_baseline} to {freq_a} (band switch after knob turn)")
            self._set_freq_gen += 1  # cancel any remaining retries
            return
        try:
            self._rig.set_frequency(hz, vfo)
        except Exception as exc:
            print(f"[_retry_set_freq] exception: {exc}")
        if attempt < 3:  # retry up to 2 times (1 second) for OmniRig not-responding
            self._root.after(500, lambda h=hz, v=vfo, g=gen, b=freq_a_baseline, a=attempt+1: self._retry_set_freq(h, v, g, b, a))

    def _on_no_base_freq(self) -> None:
        """Called when the Arduino reports it has no base frequency yet."""
        self._set_knob_report("Knob turned but no base frequency received yet", ok=False)
        self._knob_status_until = time.monotonic() + 3.0

    def _on_btn_press(self, n: int) -> None:
        """Called when ESP32 extra button n (1–4) is pressed — play voice message."""
        cmd = self._rig.get_voice_msg_command(n)
        if cmd:
            self._rig.send_cat_command(cmd)

    def _on_set_split(self, enabled: bool) -> None:
        """Called when the Arduino button initiates a split ON or OFF."""
        try:
            self._rig.set_split_mode(enabled)
        except Exception as exc:
            print(f"[_on_set_split] error: {exc}")

    def _build_port_labels(self) -> None:
        """Build (or rebuild) the port label widgets inside _port_main_row."""
        for w in self._dyn_port_widgets:
            try:
                w.destroy()
            except Exception:
                pass
        self._dyn_port_widgets.clear()

        main_row = self._port_main_row
        if main_row is None:
            return

        port_widget_refs: dict[str, list[tk.Label]] = {}
        sep_pairs: list[tuple[tk.Label, tk.Label | None]] = []
        current_rig = None
        pending_sep_last: tk.Label | None = None

        if self._radio_port_details:
            for port, is_primary, rig in self._radio_port_details:
                is_first_of_new_rig = current_rig is not None and rig != current_rig
                if is_first_of_new_rig and port_widget_refs.get(current_rig):
                    pending_sep_last = port_widget_refs[current_rig][-1]
                current_rig = rig
                lbl = tk.Label(main_row, text=port,
                               fg="#0055cc" if is_primary else "black",
                               font=("Segoe UI", 11, "bold"), padx=0, pady=0, bd=0)
                lbl.pack(side=tk.LEFT, padx=(12 if is_first_of_new_rig else 4, 0))
                self._dyn_port_widgets.append(lbl)
                port_widget_refs.setdefault(rig, []).append(lbl)
                if pending_sep_last is not None:
                    sep_pairs.append((pending_sep_last, lbl))
                    pending_sep_last = None
        else:
            lbl = tk.Label(main_row, text="-", fg="#0055cc",
                           font=("Segoe UI", 11, "bold"), padx=0, pady=0, bd=0)
            lbl.pack(side=tk.LEFT)
            self._dyn_port_widgets.append(lbl)

        self._port_sep_pairs = sep_pairs
        self._port_widget_refs = port_widget_refs

    def _place_rig_labels(self) -> None:
        """Place floating rig-name labels and red separators (called after layout is computed)."""
        for w in self._dyn_rig_labels + self._dyn_separators:
            try:
                w.destroy()
            except Exception:
                pass
        self._dyn_rig_labels.clear()
        self._dyn_separators.clear()

        hdr  = self._port_rig_sel_row
        srow = self._port_status_row
        refs = getattr(self, "_port_widget_refs", {})
        pairs = getattr(self, "_port_sep_pairs", [])
        if hdr is None or srow is None:
            return

        hdr.update_idletasks()
        total_h = srow.winfo_height()
        for rig, labels in refs.items():
            if not labels:
                continue
            x1 = labels[0].winfo_rootx() - hdr.winfo_rootx()
            x2 = labels[-1].winfo_rootx() + labels[-1].winfo_width() - hdr.winfo_rootx()
            cx = (x1 + x2) // 2
            rig_text = "Rig 1" if rig == "RIG1" else "Rig 2"
            lbl = tk.Label(hdr, text=rig_text, fg="#0055cc",
                           font=("Segoe UI", 8), padx=0, pady=0, bd=0)
            lbl.place(x=cx, y=0, anchor="n")
            self._dyn_rig_labels.append(lbl)
        for last_lbl, first_lbl in pairs:
            left_edge = first_lbl.winfo_rootx() - srow.winfo_rootx()
            sep = tk.Frame(srow, width=2, height=total_h, bg="red")
            sep.place(x=left_edge - 6, y=0)
            self._dyn_separators.append(sep)

    def _refresh(self) -> None:
        # Check OmniRig status independently
        try:
            omnirig_running = self._rig.is_omnirig_running()
        except Exception:
            omnirig_running = False

        # Check Arduino/knob connection independently
        try:
            self._refresh_knob_connection_status()
            # Send both VFO frequencies and txVfo to Arduino every cycle
            if self._transport is not None and self._transport.is_connected:
                freq_a = self._rig.read_frequency("A")
                freq_b = self._rig.read_frequency("B")
                active_vfo = self._rig.get_knob_display_vfo() if hasattr(self._rig, 'get_knob_display_vfo') else "A"
                tx_vfo = self._rig.get_tx_vfo()
                split = self._rig.read_split_mode()
                split_flag = "S" if split else "N"
                if freq_a and freq_b:
                    msg = f"LCD_FREQ:{freq_a}:{freq_b}:{active_vfo}:{tx_vfo}:{split_flag}"
                    cs = 0
                    for c in msg:
                        cs ^= ord(c)
                    self._transport.write_line(f"{msg}*{cs:02X}")

        except Exception:
            pass

        # Always update profile/model/debug info
        try:
            self._refresh_profile_file_label()
            self._refresh_loaded_models_label()
            debug = self._rig.get_debug_snapshot()
            self._debug_var.set(
                f"freq_a={debug['freq_a']}, freq_b={debug['freq_b']}, split={debug['split']}, "
                f"vfo_route={debug['vfo_route']}, tx_vfo={self._rig.get_tx_vfo()}, "
                f"raw_vfo={self._rig.get_raw_param('Vfo')}, active_vfo={debug['vfo']}, knob_vfo={debug['knob_display_vfo']}, radio_knob_vfo={self._rig.get_radio_knob_vfo()}"
            )
        except Exception:
            pass

        # Improved OmniRig status logic: only show 'Active' if OmniRig is running AND valid frequency data is received
        freq_a = self._rig.read_frequency("A") if omnirig_running else None
        freq_b = self._rig.read_frequency("B") if omnirig_running else None
        freq_current = self._rig.read_current_frequency() if omnirig_running else None
        split = self._rig.read_split_mode() if omnirig_running else None
        self._refresh_row_labels(split)
        self._apply_split_visual_effect(split)

        if not omnirig_running or freq_a is None or freq_b is None:
            self._freq_fail_count += 1
            if self._freq_fail_count >= self._freq_fail_threshold:
                self._set_na_values()
            silence = time.monotonic() - self._last_omnirig_success_time
            if silence >= 3.0:
                if self._last_omnirig_report != "Not active":
                    self._set_omnirig_report("Not active", ok=False)
                    self._last_omnirig_report = "Not active"
                # Force "---" every cycle while not active — overrides any stale cache
                self._vfo_a_var.set("---------")
                self._vfo_b_var.set("---------")
        else:
            # Success
            self._last_omnirig_success_time = time.monotonic()
            self._freq_fail_count = 0
            self._omnirig_fail_count = 0
            self._update_active_vfo_display("A")
            if self._last_omnirig_report != "Active":
                self._set_omnirig_report("Active", ok=True)
                self._last_omnirig_report = "Active"
            self._radio_type_var.set(self._rig.get_radio_type())

            if time.monotonic() < self._freq_display_gate_until:
                pass  # gate active: keep display values set by _on_set_freq/_on_snap_freq
            else:
                # Gate expired: update display from radio
                self._current_freq_var.set(_fmt_hz(freq_current) if freq_current is not None else "N/A")
                split = self._rig.read_split_mode()
                if split:
                    tx_vfo = self._rig.get_tx_vfo()
                    if tx_vfo == "A":
                        self._vfo_a_var.set(_fmt_hz(freq_a))
                        self._vfo_b_var.set(_fmt_hz(freq_b))
                    elif tx_vfo == "B":
                        self._vfo_a_var.set(_fmt_hz(freq_a))
                        self._vfo_b_var.set(_fmt_hz(freq_b))
                else:
                    if not self._rig.uses_display_slot_mode() and self._rig.get_knob_display_vfo() == "B":
                        self._vfo_a_var.set(_fmt_hz(freq_b))
                        self._vfo_b_var.set(_fmt_hz(freq_a))
                    else:
                        self._vfo_a_var.set(_fmt_hz(freq_a))
                        self._vfo_b_var.set(_fmt_hz(freq_b))

        try:
            self._vfo_route_var.set(self._rig.get_vfo_route() or "N/A")
            if self._rig.uses_display_slot_mode():
                self._knob_target_var.set(
                    f"Row 1 is the knob-frequency slot; command uses slot {self._rig.get_knob_command_vfo()}; physical VFO A/B is not exposed"
                )
            else:
                self._knob_target_var.set(
                    f"Display row {self._rig.get_knob_display_vfo()} / Command VFO {self._rig.get_knob_command_vfo()}"
                )
            self._split_var.set("YES" if split else "NO" if split is not None else "N/A")
            self._log_state(freq_a, freq_b, split, self._rig.get_vfo_route())
        except Exception as exc:
            self._set_omnirig_report("Not active", ok=False)
            self._set_knob_report("Not connected", ok=False)
            self._freq_fail_count += 1
            if self._freq_fail_count >= self._freq_fail_threshold:
                self._set_na_values()

        # ── OmniRig port refresh — triggers instantly when OmniRig.ini changes ──
        mtime = omnirig_ini_mtime()
        if mtime != self._omnirig_ini_mtime:
            self._omnirig_ini_mtime = mtime
            invalidate_omnirig_cache()
            new_details = omnirig_port_details()
            if new_details != self._radio_port_details:
                self._radio_port_details = new_details
                self._build_port_labels()
                self._root.after(200, self._place_rig_labels)

        self._root.after(self._refresh_ms, self._refresh)

    def run(self) -> None:
        self._refresh()
        self._root.mainloop()
