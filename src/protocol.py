"""Protocol layer (commands and responses)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from rig_adapter import RigAdapter


def _split_command(raw: str) -> Tuple[str, Optional[str]]:
	text = raw.strip()
	if not text:
		return "", None
	if ":" not in text:
		return text.upper(), None
	command, value = text.split(":", 1)
	return command.strip().upper(), value.strip()


def _parse_step_value(value: Optional[str]) -> int:
	if value is None:
		raise ValueError("missing step value")
	text = value.strip()
	if not text:
		raise ValueError("empty step value")
	# Allow optional + prefix from encoder-style payloads (for example STEP:+1000).
	return int(text)


@dataclass
class CommandProcessor:
	rig: RigAdapter

	def handle(self, line: str) -> str:
		command, value = _split_command(line)
		print(f"DEBUG: Protocol received command='{command}' value='{value}' from line: {line}")
		if not command:
			return "ERR:EMPTY"

		try:
			if command == "PING":
				return "PONG"

			if command == "WHO":
				return "HELLO_PC:vfoStepsKnob"

			if command == "HELLO":
				return "HELLO:vfoStepsKnob:OK"

			if command == "HELLO_ARDUINO":
				return "HELLO_PC:vfoStepsKnob:OK"

			if command == "CAPS":
				return "CAPS:PING,HELLO,CAPS,STATUS,VFO,FREQ,MODE,SET_FREQ,SET_FREQ_A,SET_FREQ_B,STEP,STEP_A,STEP_B,KNOB_FREQ,SET_MODE,SET_RF_POWER,TX_ON,RX_ON"

			if command == "STATUS":
				split = self.rig.read_split_mode()
				freq_a = self.rig.read_frequency("A")
				freq_b = self.rig.read_frequency("B")
				freq_cur = self.rig.read_current_frequency()
				split_text = "YES" if split else "NO" if split is not None else "N/A"
				return (
					"STATUS:"
					f"RADIO={self.rig.get_radio_type()};"
					f"SPLIT={split_text};"
					f"ROUTE={self.rig.get_vfo_route() or 'N/A'};"
					f"KNOB={self.rig.get_knob_display_vfo()};"
					f"CMD={self.rig.get_knob_command_vfo()};"
					f"FA={freq_a if freq_a is not None else 'N/A'};"
					f"FB={freq_b if freq_b is not None else 'N/A'};"
					f"FCUR={freq_cur if freq_cur is not None else 'N/A'}"
				)

			if command == "RADIO_TYPE":
				return f"RADIO_TYPE:{self.rig.get_radio_type()}"

			if command == "VFO":
				if not value:
					return f"VFO:{self.rig.get_active_vfo()}"
				self.rig.set_active_vfo(value)
				return f"OK:VFO:{self.rig.get_active_vfo()}"

			if command == "FREQ":
				if value:
					vfo = value.upper()
					if vfo not in {"A", "B"}:
						return "ERR:VFO"
					return f"FREQ_{vfo}:{self.rig.get_frequency(vfo)}"
				active_vfo = self.rig.get_active_vfo()
				return f"FREQ:{self.rig.get_frequency(active_vfo)}"

			if command == "KNOB_FREQ":
				knob_vfo = self.rig.get_knob_display_vfo()
				return f"KNOB_FREQ:{knob_vfo}:{self.rig.get_frequency(knob_vfo)}"

			if command == "MODE":
				return f"MODE:{self.rig.get_mode()}"

			if command == "SET_FREQ":
				if value is None:
					return "ERR:MISSING_VALUE"
				hz = int(value)
				new_hz = self.rig.set_frequency(hz)
				return f"OK:SET_FREQ:{new_hz}"

			if command == "SET_FREQ_A":
				if value is None:
					return "ERR:MISSING_VALUE"
				new_hz = self.rig.set_frequency(int(value), "A")
				return f"OK:SET_FREQ_A:{new_hz}"

			if command == "SET_FREQ_B":
				if value is None:
					return "ERR:MISSING_VALUE"
				new_hz = self.rig.set_frequency(int(value), "B")
				return f"OK:SET_FREQ_B:{new_hz}"

			if command == "STEP":
				step_hz = _parse_step_value(value)
				vfo = self.rig.get_knob_command_vfo()
				new_hz = self.rig.set_frequency(self.rig.get_frequency(vfo) + step_hz, vfo)
				return f"OK:STEP:{vfo}:{new_hz}"

			if command == "STEP_A":
				step_hz = _parse_step_value(value)
				new_hz = self.rig.set_frequency(self.rig.get_frequency("A") + step_hz, "A")
				return f"OK:STEP_A:{new_hz}"

			if command == "STEP_B":
				step_hz = _parse_step_value(value)
				new_hz = self.rig.set_frequency(self.rig.get_frequency("B") + step_hz, "B")
				return f"OK:STEP_B:{new_hz}"

			if command == "SET_MODE":
				if value is None:
					return "ERR:MISSING_VALUE"
				mode = self.rig.set_mode(value)
				return f"OK:SET_MODE:{mode}"

			if command == "SET_RF_POWER":
				if value is None:
					return "ERR:MISSING_VALUE"
				power = self.rig.set_rf_power(int(value))
				return f"OK:SET_RF_POWER:{power}"

			if command == "TX_ON":
				self.rig.set_tx(True)
				return "OK:TX_ON"

			if command == "RX_ON":
				self.rig.set_tx(False)
				return "OK:RX_ON"

			if command == "SET_TX_FREQ":
				if value is None:
					return "ERR:MISSING_VALUE"
				hz = int(value)
				tx_vfo = self.rig.get_tx_vfo()
				new_hz = self.rig.set_frequency(hz, tx_vfo)
				return f"OK:SET_TX_FREQ:{tx_vfo}:{new_hz}"

			return f"ERR:UNKNOWN_COMMAND:{command}"
		except ValueError as exc:
			return f"ERR:BAD_VALUE:{exc}"
		except Exception as exc:
			return f"ERR:INTERNAL:{exc}"
