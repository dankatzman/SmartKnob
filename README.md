# OmniPyRig Modular Server

New modular project scaffold for a radio-control bridge using OmniRig + serial transport.

## Structure

- src/app.py: Entry point
- src/rig_adapter.py: OmniRig wrapper
- src/serial_transport.py: Serial scan/connect/read/write helpers
- src/protocol.py: Command parsing and dispatch
- radio_profiles.ini: Supported radio models and per-model UI/knob behavior
- tests/: Test placeholders

## Next Step

Implement core command set compatible with your existing Arduino protocol.

## Arduino Serial Protocol (current)

Line-based ASCII, one command per line, `\n` terminated.

### Arduino Identification

To be recognized automatically as the correct knob controller, the Arduino must identify itself with this exact banner:

- `HELLO_ARDUINO:VFOKNOB`

Recommended behavior for the Arduino sketch:

- Send `HELLO_ARDUINO:VFOKNOB` once on boot after Serial starts
- If the PC sends `WHO`, reply with `HELLO_ARDUINO:VFOKNOB`

The Python app will only auto-connect to a serial device that presents this banner, so radio CAT COM ports are ignored.

## Arduino Sketch

First working sketch:

- `arduino/vfoKnob_controller/vfoKnob_controller.ino`
- PlatformIO project root: `arduino/vfoKnob_controller`
- PlatformIO source file: `arduino/vfoKnob_controller/src/main.cpp`

PlatformIO environments included:

- `uno`
- `nanoatmega328`

If your board is different, change `platformio.ini` before upload.

Default wiring in the sketch:

- Encoder A -> pin 2
- Encoder B -> pin 3
- Encoder push button -> pin 4
- Status LED -> `LED_BUILTIN`

Behavior:

- Sends `HELLO_ARDUINO:VFOKNOB` on boot
- Replies to `WHO` with `HELLO_ARDUINO:VFOKNOB`
- Rotary encoder sends `STEP:+<hz>` or `STEP:-<hz>`
- Push button cycles step size through `10, 50, 100, 500, 1000, 2500, 5000 Hz`

Adjust the pin constants at the top of the sketch if your hardware uses different pins.

- `PING` -> `PONG`
- `WHO` -> `HELLO_PC:vfoStepsKnob`
- `HELLO` -> `HELLO:vfoStepsKnob:OK`
- `HELLO_ARDUINO:VFOKNOB` -> `HELLO_PC:vfoStepsKnob:OK`
- `CAPS` -> comma-separated command capability list
- `STATUS` -> compact key/value snapshot (`RADIO`, `SPLIT`, `ROUTE`, `KNOB`, `CMD`, `FA`, `FB`, `FCUR`)
- `RADIO_TYPE` -> current model string from OmniRig
- `VFO` -> active VFO (`A` or `B`)
- `FREQ` -> frequency on active VFO
- `FREQ:A` / `FREQ:B` -> frequency on explicit VFO
- `KNOB_FREQ` -> `KNOB_FREQ:<A|B>:<hz>`
- `SET_FREQ:<hz>` -> set frequency on knob-command VFO
- `SET_FREQ_A:<hz>` / `SET_FREQ_B:<hz>` -> set explicit VFO frequency
- `STEP:<hz_delta>` -> relative step on knob-command VFO (example: `STEP:+1000`, `STEP:-100`)
- `STEP_A:<hz_delta>` / `STEP_B:<hz_delta>` -> relative step on explicit VFO
- `MODE` / `SET_MODE:<value>`
- `SET_RF_POWER:<0-100>`
- `TX_ON` / `RX_ON`

Errors are returned as `ERR:<reason>`.

## Radio Profiles

Supported radio models are defined in radio_profiles.ini.

- Add the model name to the [supported_radios] models list
- Add a matching [radio.MODEL-NAME] section
- Define per-radio behavior such as knob row, command VFO, and row labels
- Unknown radios use [radio.DEFAULT] fallback when present
- In GUI mode, the app shows which profile INI file is active and lets you browse to another file
- The last selected profile INI file is remembered and reused on next startup

Optional per-radio key:

- split_command_mode = opposite_of_knob_display
	- When split is ON, software knob sends to the opposite VFO of the knob-frequency row
