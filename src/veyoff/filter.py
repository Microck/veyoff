from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from PIL import Image, ImageDraw

try:
    from Xlib import X, display
except Exception:  # pragma: no cover - optional dependency in headless env
    X = None
    display = None


@dataclass(frozen=True)
class WindowInfo:
    title: str
    x: int
    y: int
    width: int
    height: int


def load_blacklist(path: str | Path = "config/blacklist.txt") -> list[str]:
    blacklist_path = Path(path)
    if not blacklist_path.exists():
        return []
    lines = [line.strip() for line in blacklist_path.read_text().splitlines()]
    return [line for line in lines if line and not line.startswith("#")]


def title_matches_blacklist(title: str, blacklist: Sequence[str]) -> bool:
    lowered_title = title.lower()
    return any(entry.lower() in lowered_title for entry in blacklist)


def _walk_windows(node, items: list[WindowInfo]) -> None:
    try:
        children = node.query_tree().children
    except Exception:
        return

    for child in children:
        try:
            name = child.get_wm_name() or ""
            geom = child.get_geometry()
            abs_pos = child.translate_coords(node, 0, 0)
            if name and geom.width > 0 and geom.height > 0:
                items.append(
                    WindowInfo(
                        title=name,
                        x=abs_pos.x,
                        y=abs_pos.y,
                        width=geom.width,
                        height=geom.height,
                    )
                )
        except Exception:
            pass
        _walk_windows(child, items)


def enumerate_windows() -> list[WindowInfo]:
    if display is None:
        return []

    try:
        dpy = display.Display()
        root = dpy.screen().root
    except Exception:
        return []

    windows: list[WindowInfo] = []
    _walk_windows(root, windows)
    return windows


def find_blacklisted_windows(
    blacklist: Sequence[str],
    windows: Iterable[WindowInfo] | None = None,
) -> list[WindowInfo]:
    source_windows = list(windows) if windows is not None else enumerate_windows()
    return [w for w in source_windows if title_matches_blacklist(w.title, blacklist)]


def blackout_windows(image: Image.Image, windows: Sequence[WindowInfo]) -> Image.Image:
    if not windows:
        return image

    output = image.copy()
    draw = ImageDraw.Draw(output)
    for window in windows:
        left = max(0, window.x)
        top = max(0, window.y)
        right = max(left, window.x + window.width)
        bottom = max(top, window.y + window.height)
        draw.rectangle((left, top, right, bottom), fill=(0, 0, 0))
    return output


class WindowFilter:
    def __init__(self, blacklist_path: str | Path = "config/blacklist.txt") -> None:
        self.blacklist_path = Path(blacklist_path)
        self._blacklist: list[str] = load_blacklist(self.blacklist_path)

    def reload(self) -> None:
        self._blacklist = load_blacklist(self.blacklist_path)

    @property
    def blacklist(self) -> list[str]:
        return list(self._blacklist)

    def apply(self, image: Image.Image) -> Image.Image:
        if not self._blacklist:
            return image
        blocked = find_blacklisted_windows(self._blacklist)
        return blackout_windows(image, blocked)
