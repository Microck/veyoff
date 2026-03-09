from __future__ import annotations

from threading import Lock
from typing import Callable, Optional

try:
    from pynput import keyboard
except Exception:  # pragma: no cover - depends on desktop session
    keyboard = None


class FreezeController:
    def __init__(self) -> None:
        self._frozen = False
        self._lock = Lock()

    @property
    def is_frozen(self) -> bool:
        with self._lock:
            return self._frozen

    def freeze(self) -> None:
        with self._lock:
            self._frozen = True

    def unfreeze(self) -> None:
        with self._lock:
            self._frozen = False

    def toggle(self) -> bool:
        with self._lock:
            self._frozen = not self._frozen
            return self._frozen


class HotkeyManager:
    def __init__(
        self,
        freeze_controller: FreezeController,
        hotkey: str = "<ctrl>+<shift>+f",
        on_toggle: Optional[Callable[[bool], None]] = None,
    ) -> None:
        self.freeze_controller = freeze_controller
        self.hotkey = hotkey
        self.on_toggle = on_toggle
        self._listener = None
        self.available = keyboard is not None

    def _handle_toggle(self) -> None:
        frozen = self.freeze_controller.toggle()
        if self.on_toggle:
            self.on_toggle(frozen)

    def start(self) -> None:
        if self._listener is not None or keyboard is None:
            return
        try:
            self._listener = keyboard.GlobalHotKeys({self.hotkey: self._handle_toggle})
            self._listener.start()
        except Exception:
            self._listener = None

    def stop(self) -> None:
        if self._listener is None:
            return
        self._listener.stop()
        self._listener = None

    def trigger_toggle(self) -> bool:
        self._handle_toggle()
        return self.freeze_controller.is_frozen
