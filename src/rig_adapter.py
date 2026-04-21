"""Rig adapter layer (OmniRig integration)."""

from __future__ import annotations

import configparser
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


def _get_project_root() -> Path:
	"""Locate the application root directory containing radio_profiles.ini.

	Tries multiple locations in order:
	1. Python source layout: __file__ is in src/, parents[1] is project root.
	2. Nuitka onefile bundled: radio_profiles.ini extracted alongside modules.
	3. Exe directory via sys.executable (install folder).
	4. Exe directory via sys.argv[0] (fallback).
	"""
	candidates = [
		Path(__file__).resolve().parents[1],   # Python source: src/../
		Path(__file__).resolve().parent,        # Nuitka temp dir (bundled file)
		Path(sys.executable).resolve().parent,  # Exe install directory
		Path(sys.argv[0]).resolve().parent,     # Exe directory (argv fallback)
	]
	for candidate in candidates:
		try:
			if (candidate / "radio_profiles.ini").exists():
				return candidate
		except Exception:
			pass
	return Path(sys.executable).resolve().parent


PROJECT_ROOT = _get_project_root()
DEFAULT_RADIO_PROFILES_INI = PROJECT_ROOT / "radio_profiles.ini"
APP_STATE_INI = PROJECT_ROOT / "app_state.ini"
LEGAL_FREQ_TXT = PROJECT_ROOT / "legalHFfreq.txt"


def load_legal_bands() -> list[tuple[int, int, str]]:
    """Load allowed amateur radio frequency ranges from legalHFfreq.txt.
    Returns a list of (low_hz, high_hz, name) tuples, sorted by low_hz.
    """
    bands: list[tuple[int, int, str]] = []
    try:
        with open(LEGAL_FREQ_TXT, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) < 2:
                    continue
                low_hz = int(float(parts[0]) * 1_000_000)
                high_hz = int(float(parts[1]) * 1_000_000)
                name = parts[2] if len(parts) >= 3 else ""
                if low_hz < high_hz:
                    bands.append((low_hz, high_hz, name))
    except Exception as e:
        print(f"[load_legal_bands] ERROR loading {LEGAL_FREQ_TXT}: {e}")
    if bands:
        print(f"[load_legal_bands] loaded {len(bands)} bands from {LEGAL_FREQ_TXT}")
    else:
        print(f"[load_legal_bands] WARNING: no bands loaded (file={LEGAL_FREQ_TXT})")
    return sorted(bands, key=lambda b: b[0])




def _normalize_radio_key(value: str) -> str:
	return "".join(ch.lower() for ch in value if ch.isalnum())


def _opposite_vfo(vfo: str) -> str:
	return "B" if vfo == "A" else "A"


def _load_radio_profiles(profile_ini_path: Path) -> dict[str, dict[str, str]]:
	profiles: dict[str, dict[str, str]] = {}
	if not profile_ini_path.exists():
		return profiles

	parser = configparser.ConfigParser()
	try:
		parser.read(profile_ini_path, encoding="utf-8")
	except Exception:
		return profiles

	if parser.has_section("radio.DEFAULT"):
		default_values = {key: value for key, value in parser.items("radio.DEFAULT")}
		default_values.setdefault("model_name", "DEFAULT")
		profiles["__default__"] = default_values

	supported_models: set[str] = set()
	if parser.has_section("supported_radios"):
		model_list = parser.get("supported_radios", "models", fallback="")
		for item in model_list.split(","):
			clean = item.strip()
			if clean:
				supported_models.add(_normalize_radio_key(clean))

	# Strict mode: only models explicitly listed are supported.
	if not supported_models:
		return profiles

	for section_name in parser.sections():
		if not section_name.lower().startswith("radio."):
			continue
		model_name = section_name.split(".", 1)[1].strip()
		normalized = _normalize_radio_key(model_name)
		if normalized not in supported_models:
			continue
		section_values = {key: value for key, value in parser.items(section_name)}
		section_values.setdefault("model_name", model_name)
		profiles[normalized] = section_values

	return profiles


@dataclass
class RigState:
	"""Local state fallback when OmniRig is unavailable or partially supported."""

	freq_a_hz: int = 7_100_000
	freq_b_hz: int = 14_200_000
	active_vfo: str = "A"
	mode: str = "USB"
	rf_power: int = 50
	tx: bool = False
	split: bool = False
	radio_type: str = "Unknown"



class RigAdapter:
	"""Small compatibility wrapper around omnipyrig with safe fallbacks."""

	def refresh_backend(self):
		"""Re-initialize the backend (OmniRig) after reconnect."""
		self._try_init_backend()

	def set_rig_number(self, n: int) -> None:
		self._rig_number = n
		if self._backend is not None:
			set_active = getattr(self._backend, "setActiveRig", None)
			if callable(set_active):
				try:
					set_active(n)
				except Exception:
					pass

	def __init__(self, prefer_real_backend: bool = True, profile_ini_path: Optional[str] = None, rig_number: int = 1) -> None:
		self._rig_number = rig_number
		self._state = RigState()
		self._backend: Optional[Any] = None
		self._backend_name = "mock"
		self._omnirig_running_cache = False
		self._omnirig_last_check = 0.0
		self._last_wrapper_split: Optional[bool] = None
		self._last_simplex_active_vfo: str = self._state.active_vfo
		self._freq_confirmed: bool = False  # True once OmniRig returns a real non-zero frequency
		self._profile_ini_path = self._resolve_initial_profile_ini_path(profile_ini_path)
		self._radio_profile_overrides = _load_radio_profiles(self._profile_ini_path)
		self._save_last_profile_ini_path(self._profile_ini_path)
		if prefer_real_backend:
			self._try_init_backend()

	def _resolve_initial_profile_ini_path(self, profile_ini_path: Optional[str]) -> Path:
		if profile_ini_path:
			return Path(profile_ini_path).expanduser().resolve()

		parser = configparser.ConfigParser()
		if APP_STATE_INI.exists():
			try:
				parser.read(APP_STATE_INI, encoding="utf-8")
				stored = parser.get("ui", "last_profile_ini", fallback="").strip()
				if stored:
					stored_path = Path(stored).expanduser()
					if stored_path.exists():
						return stored_path.resolve()
			except Exception:
				pass

		return DEFAULT_RADIO_PROFILES_INI.resolve()

	@staticmethod
	def _save_last_profile_ini_path(profile_ini_path: Path) -> None:
		parser = configparser.ConfigParser()
		if APP_STATE_INI.exists():
			try:
				parser.read(APP_STATE_INI, encoding="utf-8")
			except Exception:
				parser = configparser.ConfigParser()
		if not parser.has_section("ui"):
			parser.add_section("ui")
		parser.set("ui", "last_profile_ini", str(profile_ini_path))
		with APP_STATE_INI.open("w", encoding="utf-8") as handle:
			parser.write(handle)

	@property
	def backend_name(self) -> str:
		return self._backend_name

	def _try_init_backend(self) -> None:
		try:
			import omnipyrig  # type: ignore
		except Exception as exc:
			# print(f"[RigAdapter] Could not import omnipyrig: {exc}")
			return

		candidates: list[Any] = []
		wrapper_ctor = getattr(omnipyrig, "OmniRigWrapper", None)
		if callable(wrapper_ctor):
			try:
				wrapper = wrapper_ctor()
				set_active = getattr(wrapper, "setActiveRig", None)
				if callable(set_active):
					set_active(self._rig_number)
				candidates.append(wrapper)
			except Exception as exc:
				# print(f"[RigAdapter] OmniRigWrapper failed: {exc}")
				pass

		for symbol in ("OmniRigX", "OmniRig"):
			ctor = getattr(omnipyrig, symbol, None)
			if callable(ctor):
				try:
					candidates.append(ctor())
				except Exception as exc:
					# print(f"[RigAdapter] {symbol} failed: {exc}")
					pass

		getter = getattr(omnipyrig, "get_omnirig", None)
		if callable(getter):
			try:
				candidates.append(getter())
			except Exception as exc:
				# print(f"[RigAdapter] get_omnirig failed: {exc}")
				pass

		for backend in candidates:
			if backend is not None:
				self._backend = backend
				self._backend_name = type(backend).__name__
				name = self._call_any(["get_radio_name", "radio_name", "name"])
				if not isinstance(name, str) or not name.strip():
					name = self._get_param("RigType")
				if isinstance(name, str) and name.strip():
					self._state.radio_type = name.strip()
				else:
					self._state.radio_type = self._backend_name
				# print(f"[RigAdapter] OmniRig backend loaded: {self._backend_name}")
				return
		# print("[RigAdapter] No OmniRig backend loaded, using mock.")

	def _is_wrapper_backend(self) -> bool:
		return self._backend is not None and hasattr(self._backend, "getParam")

	def _get_param(self, key: str) -> Any:
		if self._backend is None:
			return None
		getter = getattr(self._backend, "getParam", None)
		if not callable(getter):
			return None
		try:
			return getter(key)
		except Exception:
			return None

	def get_raw_param(self, key: str) -> Any:
		if self._is_wrapper_backend():
			return self._get_param(key)
		return None

	def get_vfo_route(self) -> Optional[str]:
		if not self._is_wrapper_backend():
			return None
		vfo_value = self._safe_int(self._get_param("Vfo"))
		if vfo_value == 128:
			return "AA"
		if vfo_value == 256:
			return "AB"
		if vfo_value == 512:
			return "BB"
		if vfo_value == 1024:
			return "BA"
		# IC-7610: raw_vfo 2048 (simplex / short-press split) and 4096 (long-press split)
		# Route is determined by split state: AA when simplex, AB when split.
		if vfo_value in (2048, 4096):
			return "AA" if not self._last_wrapper_split else "AB"
		return None

	def get_radio_knob_vfo(self) -> Optional[str]:
		"""Return which VFO the radio's front-panel knob controls.

		Derived from the raw OmniRig Vfo parameter for IC-7610:
		  2048 = radio knob on VFO A (simplex or short-press split)
		  4096 = radio knob on VFO B (long-press split, TX side)
		Returns None if the value is unrecognized.
		"""
		if not self._is_wrapper_backend():
			return None
		vfo_value = self._safe_int(self._get_param("Vfo"))
		if vfo_value == 2048:
			return "A"
		if vfo_value == 4096:
			return "B"
		return None

	def get_debug_snapshot(self) -> dict[str, Any]:
		# Always provide real values, fallback to self._state if backend is not available
		snapshot: dict[str, Any] = {
			"backend": self._backend_name,
			"vfo": getattr(self._state, "active_vfo", None),
			"vfo_route": self.get_vfo_route(),
			"freq_current": self.read_current_frequency(),
			"freq_raw": self.read_omnirig_raw_frequency(),
			"freq_a": self.read_frequency("A"),
			"freq_b": self.read_frequency("B"),
			"knob_display_vfo": self.get_knob_display_vfo(),
			"knob_command_vfo": self.get_knob_command_vfo(),
			"split": self.read_split_mode(),
			"status": self._get_param("Status") if self._is_wrapper_backend() else None,
		}
		return snapshot

	@staticmethod
	def _safe_int(value: Any) -> Optional[int]:
		try:
			return int(value)
		except Exception:
			return None

	def _read_wrapper_split_value(self) -> Optional[bool]:
		split_value = self._safe_int(self._get_param("Split"))
		split_on = self._safe_int(getattr(self._backend, "SPLIT_ON", 0x00008000))
		split_off = self._safe_int(getattr(self._backend, "SPLIT_OFF", 0x00010000))
		if split_value is not None:
			if split_on is not None and split_value == split_on:
				return True
			if split_off is not None and split_value == split_off:
				return False

		for key in ("Split", "SplitOn", "SplitMode", "IsSplit", "SplitStatus"):
			value = self._get_param(key)
			parsed = self._to_bool(value)
			if parsed is not None:
				return parsed
		return None

	def _refresh_wrapper_operating_state(self) -> None:
		if not self._is_wrapper_backend():
			return

		previous_split = self._last_wrapper_split
		split = self._read_wrapper_split_value()
		route = self.get_vfo_route()
		vfo_value = self._safe_int(self._get_param("Vfo"))

		if split is not None:
			self._state.split = split
			self._last_wrapper_split = split

		if vfo_value is None:
			return

		if route is not None:
			if split is False:
				self._state.active_vfo = route[0]
				self._last_simplex_active_vfo = self._state.active_vfo
			elif split is True:
				# For some radios OmniRig route is inconsistent in split.
				# On OFF->ON transition, trust opposite-of-simplex and keep that choice.
				if previous_split is False:
					self._state.active_vfo = _opposite_vfo(self._last_simplex_active_vfo)
				elif self._state.active_vfo not in {"A", "B"} and route in {"AB", "BA"}:
					# Only use route as a fallback if state is not initialized.
					self._state.active_vfo = route[1]
		elif split is True and previous_split is False:
			# If route is unavailable right after toggling split, still flip from simplex VFO.
			self._state.active_vfo = _opposite_vfo(self._last_simplex_active_vfo)

	def _call_any(self, names: list[str], *args: Any) -> Any:
		if self._backend is None:
			return None
		for name in names:
			member = getattr(self._backend, name, None)
			if callable(member):
				try:
					return member(*args)
				except TypeError:
					continue
				except Exception:
					return None
		return None

	def _read_attr_any(self, names: list[str]) -> Any:
		if self._backend is None:
			return None
		for name in names:
			member = getattr(self._backend, name, None)
			if member is not None and not callable(member):
				return member
		return None

	@staticmethod
	def _to_bool(value: Any) -> Optional[bool]:
		if isinstance(value, bool):
			return value
		if isinstance(value, (int, float)):
			as_int = int(value)
			if as_int == 1:
				return True
			if as_int == 0:
				return False
			return None
		if isinstance(value, str):
			clean = value.strip().lower()
			if clean.startswith("0x"):
				try:
					hex_int = int(clean, 16)
				except Exception:
					return None
				if hex_int == 1:
					return True
				if hex_int == 0:
					return False
				return None
			if clean in {"1", "true", "on", "yes", "split"}:
				return True
			if clean in {"0", "false", "off", "no", "simplex"}:
				return False
		return None

	def _normalized_radio_type(self) -> str:
		radio_type = self.get_radio_type()
		return _normalize_radio_key(radio_type)

	def _get_radio_profile_override(self) -> Optional[dict[str, str]]:
		specific = self._radio_profile_overrides.get(self._normalized_radio_type())
		if specific is not None:
			return specific
		return self._radio_profile_overrides.get("__default__")

	def get_supported_radio_models(self) -> list[str]:
		models: list[str] = []
		for profile in self._radio_profile_overrides.values():
			model_name = profile.get("model_name")
			if model_name and model_name != "DEFAULT" and model_name not in models:
				models.append(model_name)
		return sorted(models)

	def get_split_knob_role(self) -> str:
		override = self._get_radio_profile_override()
		if override is None:
			return "auto"
		role = override.get("split_knob_role", "auto").strip().lower()
		if role in {"auto", "tx", "rx"}:
			return role
		return "auto"

	def get_profile_ini_path(self) -> str:
		return str(self._profile_ini_path)

	def set_profile_ini_path(self, profile_ini_path: str) -> list[str]:
		new_path = Path(profile_ini_path).expanduser().resolve()
		if not new_path.exists():
			raise FileNotFoundError(f"Profile file not found: {new_path}")
		loaded_profiles = _load_radio_profiles(new_path)
		self._profile_ini_path = new_path
		self._radio_profile_overrides = loaded_profiles
		self._save_last_profile_ini_path(new_path)
		return self.get_supported_radio_models()

	def get_radio_type(self) -> str:
		if self._is_wrapper_backend():
			value = self._get_param("RigType")
			if isinstance(value, str) and value.strip():
				self._state.radio_type = value.strip()
			return self._state.radio_type

		value = self._call_any(["get_radio_name", "radio_name", "name"])
		if isinstance(value, str) and value.strip():
			self._state.radio_type = value.strip()
		return self._state.radio_type

	def is_omnirig_running(self, cache_seconds: float = 3.0) -> bool:
		if self._is_wrapper_backend():
			status = self._safe_int(self._get_param("Status"))
			if status is not None:
				self._omnirig_running_cache = True
				self._omnirig_last_check = time.monotonic()
				return True

		now = time.monotonic()
		if now - self._omnirig_last_check < cache_seconds:
			return self._omnirig_running_cache

		try:
			result = subprocess.run(
				[
					"tasklist",
					"/FO",
					"CSV",
					"/NH",
				],
				capture_output=True,
				text=True,
				timeout=2,
				check=False,
				creationflags=subprocess.CREATE_NO_WINDOW,
			)
			output = f"{result.stdout}\n{result.stderr}".lower()
			# Some installs use different exe names (for example OmniRig v2).
			self._omnirig_running_cache = "omnirig" in output
		except Exception:
			self._omnirig_running_cache = False

		# If omnipyrig already has a real backend object, treat OmniRig as available.
		if not self._omnirig_running_cache and self._backend is not None:
			self._omnirig_running_cache = True

		self._omnirig_last_check = now
		return self._omnirig_running_cache

	def get_active_vfo(self) -> str:
		if self._is_wrapper_backend():
			self._refresh_wrapper_operating_state()
		return self._state.active_vfo

	def get_knob_display_vfo(self) -> str:
		override = self._get_radio_profile_override()
		if override is not None:
			raw_vfo = override.get("knob_display_vfo")
			if raw_vfo:
				vfo = raw_vfo.upper()
				if vfo in {"A", "B"}:
					return vfo
		return self.get_active_vfo()

	def get_knob_command_vfo(self) -> str:
		override = self._get_radio_profile_override()
		base_vfo = self.get_knob_display_vfo()
		split_enabled = self.read_split_mode() is True
		if override is not None:
			split_mode = override.get("split_command_mode", "same_as_knob_display").strip().lower()
			if split_enabled and split_mode == "opposite_of_knob_display":
				return _opposite_vfo(base_vfo)

			raw_vfo = override.get("knob_command_vfo")
			if raw_vfo:
				vfo = raw_vfo.upper()
				if vfo in {"A", "B"}:
					return vfo
		return base_vfo

	def get_row_labels(self) -> tuple[str, str]:
		override = self._get_radio_profile_override()
		if override is not None:
			row_a_label = override.get("row_a_label", "Knob Frequency")
			row_b_label = override.get("row_b_label", "Sub Frequency")
			return row_a_label, row_b_label
		return "Knob Frequency", "Sub Frequency"

	def uses_display_slot_mode(self) -> bool:
		override = self._get_radio_profile_override()
		if override is None:
			return False
		return override.get("display_slot_mode", "false").lower() == "true"

	def set_active_vfo(self, vfo: str) -> None:
		vfo = vfo.upper().strip()
		if vfo not in {"A", "B"}:
			raise ValueError("VFO must be A or B")
		self._state.active_vfo = vfo

	def get_frequency(self, vfo: Optional[str] = None) -> int:
		value = self.read_frequency(vfo)
		if value is not None:
			return value

		selected_vfo = (vfo or self._state.active_vfo).upper()
		if selected_vfo not in {"A", "B"}:
			raise ValueError("VFO must be A or B")

		return self._state.freq_a_hz if selected_vfo == "A" else self._state.freq_b_hz

	def read_frequency(self, vfo: Optional[str] = None) -> Optional[int]:
		selected_vfo = (vfo or self._state.active_vfo).upper()
		if selected_vfo not in {"A", "B"}:
			raise ValueError("VFO must be A or B")

		if self._is_wrapper_backend():
			status = self._safe_int(self._get_param("Status"))

			# OmniRig status 4 = OnLine (radio physically responding).
			# Status 3 = port temporarily busy — return cached frequency to ride out the blip.
			# Any other status means the radio is off or not communicating.
			if status == 3 and self._freq_confirmed:
				return self._state.freq_a_hz if selected_vfo == "A" else self._state.freq_b_hz
			if status != 4:
				self._freq_confirmed = False
				return None

			f_a = self._safe_int(self._get_param("FreqA"))
			f_b = self._safe_int(self._get_param("FreqB"))

			if f_a is not None and f_a > 0:
				self._state.freq_a_hz = f_a
				self._freq_confirmed = True
			if f_b is not None and f_b > 0:
				self._state.freq_b_hz = f_b

			# Block fallback to RigState defaults until OmniRig has returned real data.
			if not self._freq_confirmed:
				return None

			return self._state.freq_a_hz if selected_vfo == "A" else self._state.freq_b_hz

		if selected_vfo == "A":
			value = self._call_any(["get_freq_a", "get_frequency_a", "GetFreqA"])
			if value is None:
				value = self._read_attr_any(["freq_a", "frequency_a", "vfo_a"])
		else:
			value = self._call_any(["get_freq_b", "get_frequency_b", "GetFreqB"])
			if value is None:
				value = self._read_attr_any(["freq_b", "frequency_b", "vfo_b"])

		getter_names = [
			"get_freq",
			"get_frequency",
			"GetFreq",
			"freq",
		]
		if value is None:
			value = self._call_any(getter_names, selected_vfo)
		if value is None:
			value = self._call_any(getter_names)

		if isinstance(value, (int, float)):
			hz = int(value)
			if hz <= 0:
				return None
			if selected_vfo == "A":
				self._state.freq_a_hz = hz
			else:
				self._state.freq_b_hz = hz
			return hz

		return None

	def read_omnirig_raw_frequency(self) -> Optional[int]:
		if self._is_wrapper_backend():
			status = self._safe_int(self._get_param("Status"))
			if status is None or status == 3:
				return None

			for key in ("Freq", "Frequency", "CurrentFreq", "RxFreq", "TxFreq"):
				value = self._safe_int(self._get_param(key))
				if value is not None and value > 0:
					return value
			return None

		value = self._call_any(["get_freq", "get_frequency", "GetFreq", "freq"])
		if isinstance(value, (int, float)):
			hz = int(value)
			if hz > 0:
				return hz
		return None

	def read_current_frequency(self) -> Optional[int]:
		# For UI diagnostics, current frequency should follow the real knob side.
		knob_vfo = self.get_knob_display_vfo()
		knob_freq = self.read_frequency(knob_vfo)
		if knob_freq is not None:
			return knob_freq

		raw_freq = self.read_omnirig_raw_frequency()
		if raw_freq is not None:
			return raw_freq

		return self.read_frequency(self.get_active_vfo())

	def get_split_mode(self) -> bool:
		value = self.read_split_mode()
		if value is not None:
			return value
		return self._state.split

	def read_split_mode(self) -> Optional[bool]:
		if self._is_wrapper_backend():
			self._refresh_wrapper_operating_state()
			return self._last_wrapper_split

		value = self._call_any(["get_split", "is_split", "split_mode", "split", "IsSplit"])
		if value is None:
			value = self._read_attr_any(["split", "is_split", "split_mode"])

		parsed = self._to_bool(value)
		if parsed is not None:
			self._state.split = parsed
			return parsed
		return None

	def get_tx_vfo(self) -> str:
		"""Return the VFO letter ('A' or 'B') that is currently the TX side."""
		split = self.read_split_mode()
		if self.uses_display_slot_mode():
			return "B" if split else "A"
		if not split:
			return self.get_knob_display_vfo()
		route = self.get_vfo_route()
		if route and len(route) == 2:
			return route[1]
		return _opposite_vfo(self.get_knob_display_vfo())

	def get_tx_frequency(self) -> Optional[int]:
		"""Return the TX frequency in Hz, or None if unavailable.

		When not in split mode the TX and RX frequencies are the same VFO.
		When split is on, the TX VFO is identified via the OmniRig route
		(route[1] = TX side) or, as a fallback, the opposite of the knob/RX VFO.
		"""
		split = self.read_split_mode()

		if self.uses_display_slot_mode():
			# IC-7300 style: OmniRig always puts the knob/RX frequency in FreqA.
			# Split ON → FreqB is the TX slot.
			if split:
				return self.read_frequency("B")
			return self.read_frequency("A")

		if not split:
			# Simplex: TX = RX = knob display VFO.
			return self.read_frequency(self.get_knob_display_vfo())

		# Split ON: use VFO route to identify the TX VFO (route[1] = TX side).
		route = self.get_vfo_route()
		if route and len(route) == 2:
			return self.read_frequency(route[1])

		# Fallback: TX is the opposite of the knob display (RX) VFO.
		return self.read_frequency(_opposite_vfo(self.get_knob_display_vfo()))

	def set_frequency(self, hz: int, vfo: Optional[str] = None) -> int:
		if hz <= 0:
			raise ValueError("Frequency must be positive")

		selected_vfo = (vfo or self._state.active_vfo).upper()
		if selected_vfo not in {"A", "B"}:
			raise ValueError("VFO must be A or B")

		if self._is_wrapper_backend():
			# Try VFO-specific setters first (OmniRig COM interface style).
			specific_name = "SetFreqA" if selected_vfo == "A" else "SetFreqB"
			specific = getattr(self._backend, specific_name, None)
			if callable(specific):
				specific(hz)
				if selected_vfo == "A":
					self._state.freq_a_hz = hz
				else:
					self._state.freq_b_hz = hz
				return hz

			# Try generic setFrequency(vfo, hz).
			setter = getattr(self._backend, "setFrequency", None)
			if callable(setter):
				setter(selected_vfo, hz)
				if selected_vfo == "A":
					self._state.freq_a_hz = hz
				else:
					self._state.freq_b_hz = hz
				return hz

			# Try setParam("FreqA"/"FreqB", hz) or setParam("Freq", hz).
			set_param = getattr(self._backend, "setParam", None)
			if callable(set_param):
				param_key = "FreqA" if selected_vfo == "A" else "FreqB"
				set_param(param_key, hz)
				if selected_vfo == "A":
					self._state.freq_a_hz = hz
				else:
					self._state.freq_b_hz = hz
				return hz

		setter_names = ["set_freq", "set_frequency", "SetFreq"]
		called = self._call_any(setter_names, selected_vfo, hz)
		if called is None:
			self._call_any(setter_names, hz)

		if selected_vfo == "A":
			self._state.freq_a_hz = hz
		else:
			self._state.freq_b_hz = hz
		return hz

	def get_mode(self) -> str:
		value = self._call_any(["get_mode", "mode", "Mode"])
		if isinstance(value, str) and value.strip():
			self._state.mode = value.strip().upper()
		return self._state.mode

	def set_mode(self, mode: str) -> str:
		clean_mode = mode.strip().upper()
		if not clean_mode:
			raise ValueError("Mode cannot be empty")
		self._call_any(["set_mode", "SetMode"], clean_mode)
		self._state.mode = clean_mode
		return self._state.mode

	def set_rf_power(self, value: int) -> int:
		if value < 0 or value > 100:
			raise ValueError("RF power must be in range 0-100")
		self._call_any(["set_rf_power", "SetPower", "set_power"], value)
		self._state.rf_power = value
		return self._state.rf_power

	def set_split_mode(self, enabled: bool) -> bool:
		if self._is_wrapper_backend():
			if enabled:
				method = getattr(self._backend, "setSplit", None)
				if callable(method):
					try:
						method(getattr(self._backend, "ON", 1))
					except Exception:
						pass
			else:
				# omnipyrig setSplit() bug: if(state) is falsy for OFF=0 so the
				# SPLIT_OFF branch is unreachable. Write Split directly on the COM rig.
				split_off = getattr(self._backend, "SPLIT_OFF", 0x00010000)
				rig = getattr(self._backend, "_rig", None)
				if rig is not None:
					try:
						rig.Split = split_off
					except Exception:
						pass
		self._state.split = enabled
		self._last_wrapper_split = enabled
		return enabled

	def read_tx_state(self) -> bool:
		"""Return True if the radio is currently transmitting (TX ON)."""
		if not self._is_wrapper_backend():
			return False
		tx_val = self._safe_int(self._get_param("Tx"))
		tx_on = self._safe_int(getattr(self._backend, "TX_ON", None))
		if tx_val is not None and tx_on is not None:
			return tx_val == tx_on
		return False

	def get_voice_stop_command(self) -> Optional[str]:
		"""Return the CAT command to stop voice message playback, or None."""
		override = self._get_radio_profile_override()
		if override is None:
			return None
		return override.get("voice_msg_stop") or None

	def get_voice_msg_command(self, n: int) -> Optional[str]:
		"""Return the CAT command string for voice message n (1–4), or None."""
		override = self._get_radio_profile_override()
		if override is None:
			return None
		return override.get(f"voice_msg_{n}") or None

	def send_cat_command(self, cmd: str) -> bool:
		"""Send a raw CAT command via OmniRig SendCustomCommand.

		Accepts ASCII (e.g. 'PB01;') or space-separated hex bytes
		(e.g. 'FE FE 94 E0 28 01 FD'). Returns True if the call succeeded.
		"""
		if not self._is_wrapper_backend():
			return False
		try:
			parts = cmd.strip().split()
			is_hex = (
				len(parts) > 1
				and all(len(p) == 2 and all(c in "0123456789abcdefABCDEF" for c in p) for p in parts)
			)
			raw = bytes(int(p, 16) for p in parts) if is_hex else cmd.encode("ascii")

			# omnipyrig OmniRigWrapper exposes setCustomCommand(cmd, reply_len, reply_end)
			m = getattr(self._backend, "setCustomCommand", None)
			if callable(m):
				m(raw, 0, b"")
				return True

			# Fallback: OmniRig COM SendCustomCommand(RigNumber, Command, ReplyLength, ReplyEnd)
			rig = getattr(self._backend, "_rig", None)
			if rig is not None:
				m = getattr(rig, "SendCustomCommand", None)
				if callable(m):
					m(self._rig_number, raw, 0, b"")
					return True
		except Exception as exc:
			print(f"[CAT] send_cat_command exception: {exc}")
		return False

	def set_tx(self, enabled: bool) -> bool:
		if self._is_wrapper_backend():
			method_name = "setTx" if enabled else "setRx"
			method = getattr(self._backend, method_name, None)
			if callable(method):
				try:
					method()
				except Exception:
					pass

		self._call_any(["set_tx", "SetTX", "set_ptt"], enabled)
		self._state.tx = enabled
		return self._state.tx
