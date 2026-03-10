# Windows-Native Architecture

This document describes the Veyoff Windows architecture -- an RFB MITM proxy
that intercepts what Veyon's master sees on the student machine.

## Goal

A single Windows-native executable that provides:

1. **Screen Freeze** (`Ctrl+Alt+F`) -- master sees a frozen frame while the
   user continues working
2. **Master Presence Overlay** -- red banner warning when a master is connected
3. **Selective Window Hiding** -- blacklisted windows stay hidden from
   what the master sees

## Architecture: RFB Man-in-the-Middle Proxy

Veyon uses a two-tier architecture on the student machine:

```
Veyon Master (teacher)
    |
    | connects to port 11100+session (Veyon custom auth)
    v
VncProxyServer (part of Veyon service)
    |
    | connects to localhost:11200+session (standard VNC/RFB)
    v
Internal UltraVNC Server (actual screen capture)
```

Veyoff inserts itself between Veyon's proxy and UltraVNC:

```
Veyon Master (teacher)
    |
    v
VncProxyServer (port 11100+session, untouched)
    |
    v
VEYOFF RFB PROXY (port 11200+session = original VNC port)
    |   Transparent forwarding in normal mode
    |   Intercepts FramebufferUpdateRequests when frozen/blacklisting
    |   Responds with captured/frozen/redacted frames
    v
Real UltraVNC (port 11250+session = redirected via registry)
```

### Setup Flow (automated, zero manual config)

1. Veyoff reads `VncServerPort` from `HKLM\SOFTWARE\Veyon Solutions\Veyon\Network`
2. Changes it to `originalPort + 50` (e.g., 11200 -> 11250)
3. Restarts `VeyonService` (1-2 second blip, looks like normal network hiccup)
4. Binds the proxy on the original port (11200+session)
5. On exit (or `Ctrl+Alt+Q`), restores the original port and restarts the service

The external Veyon server port (11100) is never changed. The master sees no
configuration difference -- just a brief connection drop during restart.

### Proxy Protocol

The proxy performs a transparent RFB handshake: it forwards version negotiation,
security type selection, VncAuth challenge-response, and ServerInit between the
client (Veyon's proxy) and server (UltraVNC) without modification. It sniffs
the ServerInit message to learn framebuffer dimensions and pixel format.

**Normal mode:** All traffic is forwarded transparently. The master sees the
real desktop.

**Freeze mode:** When `Ctrl+Alt+F` is pressed:
- `FramebufferUpdateRequest` messages from the client are intercepted
- Instead of forwarding to UltraVNC, the proxy responds with a Raw-encoded
  full frame captured at freeze time
- Server data from UltraVNC is drained (read and discarded) to prevent
  buffer backpressure
- Other client messages (key/pointer events) are still forwarded

**Blacklist mode:** When `config/blacklist.txt` contains entries:
- The proxy captures the screen itself using GDI `BitBlt`
- Blacklisted windows keep their last teacher-visible pixels in the captured frame
- The modified frame is served to the client instead of UltraVNC's output

## Hotkey Model

Uses `RegisterHotKey` for all hotkeys:

| Hotkey | Action |
|--------|--------|
| `Ctrl+Alt+F` | Toggle screen freeze |
| `Ctrl+Alt+Q` | Clean quit (restore Veyon, exit) |
| `Ctrl+Alt+X` (x5 in 2s) | Self-destruct panic button |

`Ctrl+Alt+X` uses a multi-press trigger: 5 presses within 2 seconds. Each press
is timestamped and old presses outside the window are discarded. This prevents
accidental triggers while staying fast in a real emergency. Note: unlike the
other hotkeys, `Ctrl+Alt+X` does not use `MOD_NOREPEAT` so holding the key
counts repeated presses.

## System Tray

A notification area icon provides a right-click context menu (toggle freeze,
edit blacklist, reload config, status display, self-destruct, quit). The icon
is a dynamically generated 16x16 colored square:

| Color | State |
|-------|-------|
| Green | Idle / live |
| Blue | Screen frozen |
| Amber | Veyon app connected |
| Red | Master actively viewing |

The icon and tooltip update on every poll cycle (500ms). The tray icon is
created with `Shell_NotifyIconW` and removed cleanly on exit or self-destruct.

## Self-Destruct (Nuclear)

The self-destruct sequence performs a complete trace removal:

1. **Remove tray icon** immediately (visual cleanup)
2. **Restore Veyon registry** and restart the service (normal cleanup)
3. **Clear Windows event logs** (Application, System, Security channels via
   `ClearEventLogW`)
4. **Delete prefetch entries** matching "veyoff" in `%WINDIR%\Prefetch`
5. **Delete config files** (blacklist.txt, config directory)
6. **Schedule self-deletion**: writes a hidden batch script to `%TEMP%` that
   polls `tasklist` for the veyoff PID, waits for exit, then `rd /s /q` the
   entire exe directory and deletes the batch script itself
7. **Exit immediately** via `ExitProcess(0)`

The batch-based self-deletion is necessary because a running exe cannot delete
itself on Windows. The script runs as a detached, hidden `cmd.exe` process.

## Master Presence Detection

Polls `GetExtendedTcpTable` for established TCP connections on Veyon ports
(5900, 11100+session, 11200+session). This is more accurate than process
name guessing.

## Overlay

A topmost layered banner window with `WDA_EXCLUDEFROMCAPTURE` so Veyon's
capture never sees it. Uses `LWA_ALPHA` (not `LWA_COLORKEY`) to avoid
triggering Veyon's interfering window detection.

**Veyon's anti-cheat** (`inspectDesktopWindows`) checks every 1 second for
windows with `WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED` AND
`LWA_COLORKEY`. Since our overlay uses only `LWA_ALPHA`, it is not detected.

## Blacklist

Plain text file (`config/blacklist.txt`), one keyword per line. Window titles
containing any keyword (case-insensitive) stay hidden from the master by
preserving the last teacher-visible pixels in that region. The file is
hot-reloaded when modified.

## Build

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Requires: Windows SDK, MSVC, CMake 3.21+.
Links: ws2_32, advapi32, dwmapi, gdi32, iphlpapi, shell32, user32.

## Registry Keys

| Path | Value | Default |
|------|-------|---------|
| `HKLM\SOFTWARE\Veyon Solutions\Veyon\Network\VncServerPort` | REG_DWORD | 11200 |
| `HKLM\SOFTWARE\Veyon Solutions\Veyon\Network\VeyonServerPort` | REG_DWORD | 11100 |
| `HKLM\SOFTWARE\Veyon Solutions\Veyon\Windows\InterferingWindowsHandling` | REG_DWORD | 0 (None) |

## Service

- Name: `VeyonService`
- Display name: `Veyon Service`
- Control: `net stop/start VeyonService` or `sc` commands
