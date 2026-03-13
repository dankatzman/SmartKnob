# Project Notes

## What exists now

- Original working reference script remains untouched:
  - `c:\Dropbox\LAB_X230_PC\Python Projects\StepSL_OmniPyRig_Server2.py`
- New separate project scaffold created here:
  - `c:\Dropbox\LAB_X230_PC\Arduino Projects\voicekyerio\omnipyrig_server_modular`

## New project structure

- `src/app.py`
- `src/rig_adapter.py`
- `src/serial_transport.py`
- `src/protocol.py`
- `tests/`
- `requirements.txt`

## Decisions made

- Do not modify the original Python script.
- Build the new system as a separate modular Python project.
- The new project will use `omnipyrig` to communicate with OmniRig.
- OmniRig must already be installed/configured on Windows.
- `omnipyrig` is a Python dependency that must be installed and imported.

## Architecture direction

Separate responsibilities into modules:

- Rig adapter: OmniRig read/write functions
- Serial transport: port discovery, handshake, reconnect, read/write
- Protocol: command parsing and response formatting
- App/main: orchestration
- Optional UI layer later if needed

## Likely protocol compatibility goal

Keep compatibility with the current Arduino-side command style where practical:

- `FREQ`
- `RADIO_TYPE`
- `MODE`
- `SET_FREQ:<value>`
- `SET_MODE:<value>`
- `SET_RF_POWER:<value>`
- `TX_ON`
- `RX_ON`

## Important reminder

The user wants to describe the new project requirements first before more implementation happens.
Ask clarifying questions where requirements are incomplete.

## Suggested next session start

1. Ask the user to describe the new project goal, hardware, command set, and data flow.
2. Confirm which existing behaviors must remain compatible.
3. Then implement the first working modular version.

### Goal
the new project is called vfoStepsKnob
the mission of the project is to build an external knob that controls the VFO frequency if an HF amateure transciver
an hf transciver is connected via its usb to a computer.
on the computer thaere is a program called omnirig that enables a comunication between the radio and a log software
this control type is called CAT in the world of amateur radio or as they call it ham radio.
omnirig can be called from more that one software so beside enableing the log software to control the radio it enables other
foftwares to talk with the radio as well.
there is a program called omnipyrig that can me included into a python program and serves as the comunicator between the pyton
code to the omnirig and therefor enables talking to the radio as well.
my plane is to write a python program taht will run on the pc that omnirig and the loging software is running and will
enable me to read data and send commands to the radio.
this python program will talk over a com port with and arduino based device and will enable me to send from the arduino a command to change things in the radio . mainly the frequencies of the VFO of the radio. vfoA and vfoB.
this python program is called vfoStepsKnob.
 


### Hardware
List radio, Arduino, PC, serial links, other devices.

### Required Features
- 
- 
- 

### Data Flow
Explain who sends commands to whom and when.

### Compatibility Requirements
List anything that must remain the same as the current script.

### Improvements Wanted
List what should be cleaner or better than the old script.

### Version 1 Priorities