"""Serial transport layer (port scan/connect/io)."""

from __future__ import annotations

import time
from typing import Optional

import serial
from serial.tools import list_ports


ARDUINO_ID_BANNER = "HELLO_ARDUINO:VFOKNOB"


def _port_haystack(info: object) -> str:
	return (
		f"{getattr(info, 'device', '')} "
		f"{getattr(info, 'name', '')} "
		f"{getattr(info, 'description', '')} "
		f"{getattr(info, 'manufacturer', '')} "
		f"{getattr(info, 'product', '')} "
		f"{getattr(info, 'interface', '')} "
		f"{getattr(info, 'hwid', '')}"
	).lower()


def _score_port(info: object) -> int:
	haystack = _port_haystack(info)
	for keyword in ("ic-7300", "icom", "kenwood", "yaesu", "elecraft", "flexradio"):
		if keyword in haystack:
			return -10

	score = 0
	for keyword in ("arduino", "ch340", "wch", "ftdi", "usb serial", "2341", "2a03", "1a86", "0403"):
		if keyword in haystack:
			score += 1
	return score


class SerialTransport:
	"""Thin wrapper around pyserial for newline-based command exchange."""

	def __init__(self) -> None:
		self._ser: Optional[serial.Serial] = None

	@property
	def is_connected(self) -> bool:
		return self._ser is not None and self._ser.is_open

	def open(self, port: str, baudrate: int, timeout_s: float = 0.25) -> None:
		self.close()
		self._ser = serial.Serial(port=port, baudrate=baudrate, timeout=timeout_s)

	def close(self) -> None:
		if self._ser is not None:
			try:
				self._ser.close()
			finally:
				self._ser = None

	@staticmethod
	def candidate_ports(port_hint: Optional[str] = None) -> list[str]:
		if port_hint:
			return [port_hint]

		ports = list(list_ports.comports())
		if not ports:
			return []

		scored = [(_score_port(info), info.device) for info in ports]
		scored.sort(key=lambda item: item[0], reverse=True)
		return [device for _, device in scored]

	@staticmethod
	def probe_device_identity_details(port: str, baudrate: int, timeout_s: float = 4.5) -> tuple[bool, str | None]:
		try:
			with serial.Serial(port=port, baudrate=baudrate, timeout=0.2, write_timeout=0.2) as handle:
				# Many Arduinos reset on open; allow time for boot banner.
				start = time.monotonic()
				banner_deadline = start + timeout_s
				next_query_at = start + 2.4

				while time.monotonic() < banner_deadline:
					raw = handle.readline()
					if raw:
						text = raw.decode("utf-8", errors="ignore").strip()
						if text == ARDUINO_ID_BANNER or ARDUINO_ID_BANNER in text:
							return True, None

					now = time.monotonic()
					if now >= next_query_at:
						try:
							handle.write(b"WHO\n")
						except Exception:
							pass
						next_query_at = now + 0.7

				return False, "Banner not received"
		except Exception as exc:
			return False, str(exc)

	@staticmethod
	def probe_device_identity(port: str, baudrate: int, timeout_s: float = 4.5) -> bool:
		matched, _ = SerialTransport.probe_device_identity_details(port, baudrate, timeout_s=timeout_s)
		return matched

	def auto_connect(self, baudrate: int, port_hint: Optional[str] = None) -> str:
		ports = self.candidate_ports(port_hint=port_hint)
		if not ports:
			raise RuntimeError("No serial ports found")

		for candidate in ports:
			if self.probe_device_identity(candidate, baudrate):
				self.open(candidate, baudrate)
				return candidate

		if port_hint:
			raise RuntimeError(f"Configured port {port_hint} did not identify as vfoKnob Arduino")
		raise RuntimeError("No verified vfoKnob Arduino found")

	def read_line(self) -> Optional[str]:
		if not self.is_connected or self._ser is None:
			return None
		raw = self._ser.readline()
		if not raw:
			return None
		text = raw.decode("utf-8", errors="ignore").strip()
		return text or None

	def write_line(self, line: str) -> None:
		if not self.is_connected or self._ser is None:
			raise RuntimeError("Serial link is not connected")
		payload = (line.rstrip("\r\n") + "\n").encode("utf-8")
		self._ser.write(payload)
