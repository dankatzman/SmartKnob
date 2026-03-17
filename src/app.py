"""Application entry point for OmniPyRig Modular Server."""

from __future__ import annotations

import argparse
import time


from pathlib import Path
from gui_monitor import RigMonitorWindow, LogWindow
from protocol import CommandProcessor
from rig_adapter import RigAdapter
from serial_transport import SerialTransport


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="vfoStepsKnob")
    parser.add_argument("--port", default=None, help="COM port, e.g. COM4")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument(
        "--serial",
        action="store_true",
        help="Run Arduino serial bridge mode",
    )
    parser.add_argument(
        "--gui",
        action="store_true",
        help="Run desktop monitor window mode (default)",
    )
    parser.add_argument(
        "--stdin",
        action="store_true",
        help="Read commands from stdin instead of serial (development mode)",
    )
    parser.add_argument(
        "--mock",
        action="store_true",
        help="Disable real OmniRig backend and use local mock state only",
    )
    return parser


def _run_stdin_mode(processor: CommandProcessor) -> None:
    print("stdin mode ready. Type protocol commands (Ctrl+C to exit).")
    while True:
        try:
            line = input("> ").strip()
        except EOFError:
            print("stdin closed.")
            return
        if not line:
            continue
        print(processor.handle(line))


def _run_serial_mode(processor: CommandProcessor, port: str | None, baud: int) -> None:
    transport = SerialTransport()
    print("Serial bridge mode. Waiting for Arduino... (Ctrl+C to stop)")
    last_wait_notice = 0.0

    try:
        while True:
            if not transport.is_connected:
                now = time.monotonic()
                if now - last_wait_notice >= 3.0:
                    print("Searching for Arduino serial port...")
                    last_wait_notice = now

                try:
                    chosen = transport.auto_connect(baudrate=baud, port_hint=port)
                    print(f"Serial connected: {chosen} @ {baud}")
                    print("Bridge running.")
                    # Optional handshake for Arduino sketches that implement HELLO.
                    try:
                        transport.write_line("HELLO")
                    except Exception:
                        pass
                except Exception:
                    time.sleep(1.0)
                    continue

            try:
                line = transport.read_line()
                if line is None:
                    continue

                print(f"DEBUG: Received line from Arduino: {line}")
                response = processor.handle(line)
                transport.write_line(response)
                print(f"RX: {line} | TX: {response}")
            except Exception as exc:
                print(f"Serial link lost ({exc}). Reconnecting...")
                transport.close()
                time.sleep(0.8)
    finally:
        transport.close()


def main() -> None:
    args = _build_parser().parse_args()
    rig = RigAdapter(prefer_real_backend=not args.mock)
    print(f"Rig backend: {rig.backend_name}")
    if args.stdin:
        processor = CommandProcessor(rig)
        _run_stdin_mode(processor)
    elif args.serial:
        processor = CommandProcessor(rig)
        _run_serial_mode(processor, args.port, args.baud)
    else:
        log_path = str((Path(__file__).resolve().parents[1] / "freq_log.txt"))
        window = RigMonitorWindow(rig=rig, refresh_ms=150, log_path=log_path)
        def show_log_window():
            try:
                window._log_window = LogWindow(log_path, parent=window._root, session_start=window._log_session_start)
            except Exception as e:
                print(f"Could not open log window: {e}")
        window._log_window = None
        window._root.after(500, show_log_window)
        window.run()
        import sys
        sys.exit(0)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
