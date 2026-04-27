from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

import mss
from PIL import Image

from .filter import WindowFilter


@dataclass
class CachedFrame:
    frame: Image.Image


class ScreenCapture:
    def __init__(self, window_filter: Optional[WindowFilter] = None) -> None:
        self._window_filter = window_filter
        self._cached_frame: Optional[CachedFrame] = None

    def capture_screen(self) -> Image.Image:
        try:
            with mss.mss() as sct:
                monitor = sct.monitors[0]
                shot = sct.grab(monitor)
                frame = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
        except Exception:
            frame = Image.new("RGB", (1280, 720), color=(0, 0, 0))

        if self._window_filter:
            self._window_filter.reload()
            frame = self._window_filter.apply(frame)

        self.save_frame_to_buffer(frame)
        return frame

    def save_frame_to_buffer(self, frame: Image.Image) -> None:
        self._cached_frame = CachedFrame(frame=frame.copy())

    def get_cached_frame(self) -> Optional[Image.Image]:
        if self._cached_frame is None:
            return None
        return self._cached_frame.frame.copy()

    def get_output_frame(self, frozen: bool) -> Image.Image:
        if frozen:
            cached = self.get_cached_frame()
            if cached is not None:
                return cached
        return self.capture_screen()
