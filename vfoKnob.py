"""Launcher script for vfoStepsKnob."""

from __future__ import annotations

import pathlib
import sys


def _bootstrap_src_path() -> None:
	root = pathlib.Path(__file__).resolve().parent
	src = root / "src"
	if str(src) not in sys.path:
		sys.path.insert(0, str(src))


def main() -> None:
	_bootstrap_src_path()
	from app import main as app_main

	app_main()


if __name__ == "__main__":
	main()
