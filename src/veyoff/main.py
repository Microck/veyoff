from __future__ import annotations

import argparse
import signal
import time
from pathlib import Path

from .capture import ScreenCapture
from .filter import WindowFilter
from .hotkey import FreezeController, HotkeyManager
from .overlay import PresenceOverlay, detect_master_connection


def _status_from_state(frozen: bool, master_active: bool) -> str:
    if frozen and master_active:
        return "FROZEN | MASTER VIEWING"
    if frozen:
        return "FROZEN"
    if master_active:
        return "LIVE | MASTER VIEWING"
    return "LIVE"


def run_capture_loop(
    output_path: Path = Path("/tmp/veyoff_test.png"),
    interval_seconds: float = 0.1,
    overlay_poll_seconds: float = 1.0,
    enable_hotkey: bool = True,
    headless_overlay: bool = False,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    freeze_controller = FreezeController()

    def on_toggle(frozen: bool) -> None:
        print("FROZEN" if frozen else "UNFROZEN", flush=True)

    hotkey_manager = HotkeyManager(freeze_controller, on_toggle=on_toggle)
    if enable_hotkey:
        hotkey_manager.start()

    window_filter = WindowFilter()
    capture = ScreenCapture(window_filter=window_filter)
    overlay = PresenceOverlay(headless=headless_overlay)

    running = True

    def stop_handler(*_args) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)

    next_overlay_poll = 0.0
    master_active = False
    last_status = ""

    try:
        while running:
            now = time.monotonic()
            if now >= next_overlay_poll:
                master_active = detect_master_connection()
                overlay.update(master_active)
                next_overlay_poll = now + overlay_poll_seconds

            frame = capture.get_output_frame(freeze_controller.is_frozen)
            frame.save(output_path)

            status = _status_from_state(freeze_controller.is_frozen, master_active)
            if status != last_status:
                print(status, flush=True)
                last_status = status

            time.sleep(interval_seconds)
    finally:
        hotkey_manager.stop()
        overlay.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Veyoff screen capture loop")
    parser.add_argument("--output", default="/tmp/veyoff_test.png")
    parser.add_argument("--interval", type=float, default=0.1)
    parser.add_argument("--overlay-interval", type=float, default=1.0)
    parser.add_argument("--no-hotkey", action="store_true")
    parser.add_argument("--headless-overlay", action="store_true")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    run_capture_loop(
        output_path=Path(args.output),
        interval_seconds=args.interval,
        overlay_poll_seconds=args.overlay_interval,
        enable_hotkey=not args.no_hotkey,
        headless_overlay=args.headless_overlay,
    )


if __name__ == "__main__":
    main()
