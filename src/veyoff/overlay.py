from __future__ import annotations

import subprocess
from dataclasses import dataclass
from threading import Lock, Thread
from typing import Iterable, Sequence

DEFAULT_MASTER_PORTS = (5900, 11100, 11101)


@dataclass(frozen=True)
class TcpConnection:
    local_port: int
    remote_port: int
    state: str


def _parse_ss_output(output: str) -> list[TcpConnection]:
    connections: list[TcpConnection] = []
    for line in output.splitlines():
        line = line.strip()
        if not line or line.startswith("State"):
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        state = parts[0].upper()
        local = parts[3]
        remote = parts[4]
        try:
            local_port = int(local.rsplit(":", 1)[1])
            remote_port = int(remote.rsplit(":", 1)[1])
        except Exception:
            continue
        connections.append(TcpConnection(local_port, remote_port, state))
    return connections


def read_tcp_connections() -> list[TcpConnection]:
    try:
        result = subprocess.run(
            ["ss", "-tn"],
            capture_output=True,
            check=False,
            text=True,
        )
        if result.returncode == 0:
            return _parse_ss_output(result.stdout)
    except Exception:
        return []
    return []


def detect_master_connection(
    ports: Sequence[int] = DEFAULT_MASTER_PORTS,
    connections: Iterable[TcpConnection] | None = None,
) -> bool:
    active_connections = (
        list(connections) if connections is not None else read_tcp_connections()
    )
    port_set = set(ports)
    for conn in active_connections:
        if conn.state not in {"ESTAB", "ESTABLISHED"}:
            continue
        if conn.local_port in port_set or conn.remote_port in port_set:
            return True
    return False


class PresenceOverlay:
    def __init__(self, message: str = "MASTER VIEWING", headless: bool = False) -> None:
        self.message = message
        self.headless = headless
        self._visible = False
        self._lock = Lock()
        self._thread: Thread | None = None
        self._root = None
        self._label = None

    @property
    def visible(self) -> bool:
        with self._lock:
            return self._visible

    def _set_visible(self, value: bool) -> None:
        with self._lock:
            self._visible = value

    def _start_gui(self) -> None:
        if self.headless or self._thread is not None:
            return
        self._thread = Thread(target=self._run_tk_loop, daemon=True)
        self._thread.start()

    def _run_tk_loop(self) -> None:
        try:
            import tkinter as tk
        except Exception:
            self.headless = True
            return

        try:
            root = tk.Tk()
            root.overrideredirect(True)
            root.attributes("-topmost", True)
            root.configure(bg="red")
            root.geometry("520x70+25+25")

            label = tk.Label(
                root,
                text=self.message,
                font=("Helvetica", 22, "bold"),
                bg="red",
                fg="white",
            )
            label.pack(fill="both", expand=True)
            root.withdraw()

            self._root = root
            self._label = label
            root.mainloop()
        except Exception:
            self.headless = True

    def show(self) -> None:
        self._set_visible(True)
        if self.headless:
            return
        self._start_gui()
        if self._root:
            self._root.after(0, self._root.deiconify)

    def hide(self) -> None:
        self._set_visible(False)
        if self.headless:
            return
        if self._root:
            self._root.after(0, self._root.withdraw)

    def update(self, active: bool) -> None:
        if active:
            self.show()
        else:
            self.hide()

    def close(self) -> None:
        self._set_visible(False)
        if self._root:
            self._root.after(0, self._root.destroy)
