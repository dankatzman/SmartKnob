# SmartKnob — Arduino VFO Controller for Ham Radio

A hardware rotary encoder knob that connects to your Windows PC and controls your transceiver's VFO frequency in real time, via OmniRig. Includes a 16×2 LCD display, band-edge protection, split-mode support, and a Python GUI monitor window.

Supported radios: **Icom IC-7300**, **Icom IC-7610**, **Yaesu FTDX10**, and any radio supported by OmniRig.

---

## Features

- **Hardware tuning knob** — rotary encoder with quadrature ISR, one step per physical click
- **16×2 LCD display** — shows VFO A & B frequencies, step size, split offset, and knob indicator
- **Split mode** — auto-detects RX/TX split, snaps TX frequency to configured offset, shows live offset on display
- **Band validation** — legal HF amateur band table loaded from file; tuning stops at band edges with blinking LED warning
- **VIP snap** — if frequency is out-of-band, first knob click snaps to nearest band edge in direction of turn
- **Persistent settings** — step size (500 Hz / 1 kHz) and split range stored in Arduino EEPROM
- **Python GUI monitor** — always-on-top window showing radio state, VFO frequencies, split status, and connection health
- **Auto port detection** — Python finds the Arduino automatically; avoids radio CAT ports
- **Auto-reconnect** — reconnects automatically if USB is unplugged and re-plugged
- **XOR checksum** — LCD update packets are checksummed; corrupted packets are silently discarded
- **Multi-radio profiles** — per-radio configuration in `radio_profiles.ini`
- **Windows installer** — single EXE built with Nuitka + Inno Setup

---

## Hardware

### Components

| Component | Detail |
|-----------|--------|
| Arduino Uno | ATmega328P, communicates via built-in USB |
| Rotary encoder | Quadrature type with push button (e.g. EC11) |
| 16×2 I2C LCD | Address 0x27 (or 0x3F), LiquidCrystal_I2C library |
| Toggle switch | Selects knob target: RX VFO or TX VFO |
| LEDs | RX indicator (pin 11), TX indicator (pin 12), comm indicator (pin 10) |
| Decoupling caps | 0.1 µF from encoder CLK and DT pins to GND (hardware debounce) |

### Pin Assignments

| Pin | Function |
|-----|----------|
| 2 (INT0) | Encoder CLK (A phase) |
| 3 (INT1) | Encoder DT (B phase) |
| 4 | Encoder push button |
| 6 | Knob target switch (HIGH = TX, LOW = RX) |
| 10 | Communication LED (lit when Python is connected) |
| 11 | RX indicator LED |
| 12 | TX indicator LED |
| A4 / A5 | I2C SDA / SCL for LCD |

---

## LCD Display Layout

```
Col:  0   1─────────9  10──────15
     ┌───┬──────────┬───────────┐
R0:  │ ✦ │  7.100.0 │   5- 8   │  ← VFO A freq │ Split range (FROM-UP)
     ├───┼──────────┼───────────┤
R1:  │   │ 14.200.0 │     1K   │  ← VFO B freq │ Step size (or split offset)
     └───┴──────────┴───────────┘
```

- **Col 0**: Knob indicator (custom character) — marks which row the knob is tuning
- **Col 1–9**: Frequency formatted as `MM.KKK.H` (e.g., ` 7.100.0` = 7.100.0 MHz)
- **Col 10–15 row 0**: Split range `FROM-UP` in kHz (e.g., ` 5- 8`)
- **Col 10–15 row 1**: Step size (`   1K` / `  .5K`) or split offset (`+2.0K`) in split mode

---

## Python GUI

The Python app shows a monitor window with:

- Radio model (IC-7300, IC-7610, FTDX10…)
- VFO A and VFO B frequencies with dynamic labels (RX/TX color coded)
- Split mode indicator (YES/NO)
- VFO route (AA / AB / BA / BB)
- Knob target (which VFO the hardware knob controls)
- OmniRig connection status
- Arduino connection status

---

## Button Functions

| Action | Result |
|--------|--------|
| Short press (normal mode) | Sends `BTN:PRESS` to Python (configurable action) |
| Long press (>1 second) | Enters parameter edit mode |
| Short press (edit mode) | Cycles between STEP / FROM / UP parameters |
| Long press (edit mode) | Saves parameters to EEPROM, exits edit mode |
| Knob turn (edit mode) | Adjusts the selected parameter |

### Editable Parameters

| Parameter | Description | Range |
|-----------|-------------|-------|
| STEP | Frequency step per click | 500 Hz / 1 kHz |
| FROM | Split range lower offset | 1–99 kHz |
| UP | Split range upper offset | 1–99 kHz |

---

## Requirements

### Hardware
- Arduino Uno (or compatible ATmega328P board)
- OmniRig installed and configured for your radio

### Software
- Windows 10 or later
- [OmniRig](http://www.dxatlas.com/OmniRig/) (configured for your radio)
- Python 3.10+ (for running from source)
- PlatformIO (for flashing the Arduino firmware)

### Python Dependencies (from source)
```
pip install -r requirements.txt
```

---

## Installation

### Option A — Windows Installer (recommended)

Download `SmartKnobSetup.exe` from the [Releases](../../releases) page and run it.
The installer places `smartknob.exe` in `C:\Program Files\SmartKnob` and creates a desktop shortcut.

### Option B — From source

```bash
git clone https://github.com/dankatzman/SmartKnob.git
cd SmartKnob
pip install -r requirements.txt
python vfoKnob.py
```

### Flashing the Arduino

1. Open the project in VSCode with the PlatformIO extension
2. Open `arduino/vfoKnob_controller/`
3. Click the **Upload** arrow in the PlatformIO status bar
4. After upload completes, the port is free — Python connects automatically

> **Note:** The Arduino communicates via its built-in USB port (hardware serial). No external USB-to-serial adapter is needed.

---

## Configuration

### Radio Profiles (`radio_profiles.ini`)

Each section defines a radio model. The profile is selected automatically based on what OmniRig reports.

```ini
[IC-7300]
display_slot_mode = true
split_command_vfo = opposite
row_a_label = VFO A
row_b_label = VFO B
```

### Legal Bands (`legalHFfreq.txt`)

Defines the amateur HF bands used for band-edge protection. Format:

```
low_mhz  high_mhz  name
1.8      2.0       160m
3.5      4.0       80m
...
```

---

## Architecture

```
┌─────────────────────────────────┐      USB (hardware serial, 57600 baud)
│         Python (Windows PC)     │ ◄──────────────────────────────────────►
│                                 │
│  vfoKnob.py → app.py            │      OmniRig (COM/CAT)
│  gui_monitor.py  (Tkinter GUI)  │ ◄──────────────────────────────────────►
│  rig_adapter.py  (OmniRig)      │                 Radio
│  serial_transport.py            │
│  protocol.py                    │
│  radio_poller.py                │
└─────────────────────────────────┘

┌─────────────────────────────────┐
│       Arduino Uno               │
│                                 │
│  Rotary encoder (pins 2, 3)     │
│  Push button   (pin 4)          │
│  Target switch (pin 6)          │
│  I2C LCD       (A4, A5)         │
│  LEDs          (10, 11, 12)     │
└─────────────────────────────────┘
```

---

## Serial Protocol

Communication uses newline-terminated ASCII commands at 57600 baud with optional XOR checksum on frequency update packets.

### Key Commands (Python → Arduino)

| Command | Description |
|---------|-------------|
| `LCD_FREQ:<A>:<B>:<activeVfo>:<txVfo>*<checksum>` | Update LCD display |
| `BAND_CLEAR` | Reset band table |
| `BAND_ADD:<low_hz>:<high_hz>` | Add a legal band |
| `WHO` | Request identity banner |
| `PING` | Connectivity check |

### Key Messages (Arduino → Python)

| Message | Description |
|---------|-------------|
| `HELLO_ARDUINO:VFOKNOB` | Boot banner / identity |
| `SET_FREQ:<hz>:<vfo>` | Frequency change from knob |
| `SNAP_FREQ:<hz>:<vfo>` | Snap request (split mode) |
| `BTN:PRESS` | Button press event |
| `PONG_ARDUINO` | Response to PING |

---

## Building the Installer

Requires [Nuitka](https://nuitka.net/) and [Inno Setup](https://jrsoftware.org/isinfo.php).

```bat
build_installer.bat
```

Output: `dist/SmartKnobSetup.exe`

---

## License

MIT License — see [LICENSE](LICENSE) for details.
