from __future__ import annotations

import os

from .main import run_capture_loop


def main() -> None:
    headless = not bool(os.environ.get("DISPLAY"))
    run_capture_loop(enable_hotkey=True, headless_overlay=headless)


if __name__ == "__main__":
    main()
