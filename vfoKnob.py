from __future__ import annotations

def create_desktop_shortcut():
	if getattr(sys, 'frozen', False):
		try:
			import win32com.client
			desktop = os.path.join(os.path.join(os.environ['USERPROFILE']), 'Desktop')
			shortcut_path = os.path.join(desktop, "SmartKnob.lnk")
			target = sys.executable
			wsh = win32com.client.Dispatch("WScript.Shell")
			shortcut = wsh.CreateShortcut(shortcut_path)
			shortcut.TargetPath = target
			shortcut.WorkingDirectory = os.path.dirname(target)
			shortcut.IconLocation = target
			shortcut.save()
		except Exception:
			pass

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "src"))
"""Launcher script for vfoStepsKnob."""

import pathlib

def _bootstrap_src_path() -> None:
	root = pathlib.Path(__file__).resolve().parent
	src = root / "src"
	if str(src) not in sys.path:
		sys.path.insert(0, str(src))


def main() -> None:
	_bootstrap_src_path()
	create_desktop_shortcut()
	from app import main as app_main
	app_main()


if __name__ == "__main__":
	main()
