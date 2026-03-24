# Windows Build and Validation

## Prerequisites

- Windows 10 or later
- Visual Studio 2022 (or MinGW-w64 with C++20 support)
- CMake 3.21+
- **Administrator privileges** (required for registry modification and service control)

## Build

### Visual Studio Developer Prompt

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build --config Release
```

### Or with Visual Studio generator

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Run

**Must be run as Administrator** (right-click -> Run as administrator, or from
an elevated command prompt).

```powershell
.\build\veyoff-windows.exe
```

Or with a custom blacklist path:

```powershell
.\build\veyoff-windows.exe --blacklist C:\path\to\blacklist.txt
```

### What happens on launch

1. Reads `VncServerPort` from Veyon's registry
2. Redirects it (11200 -> 11250) and restarts `VeyonService`
3. Starts RFB proxy on the original port
4. Creates the overlay window (hidden until master connects)
5. Prints status to console

### What happens on exit (`Ctrl+Alt+Q`)

1. Restores the original VNC port in registry
2. Restarts `VeyonService` to apply the restoration
3. Exits cleanly

## Manual Validation

### 1. Freeze Hotkey

1. Start `veyoff-windows.exe` as admin
2. Have Veyon running (master connects as usual)
3. Press `Ctrl+Alt+F` -- console prints `FROZEN`
4. Change something on screen -- master should still see the old frame
5. Press `Ctrl+Alt+F` again -- console prints `LIVE`, master sees real desktop

### 2. Master Presence Overlay

1. When a Veyon master connects, a thin red inside outline appears around the
   visible monitor bounds
2. If the master app is connected but not actively viewing, the outline is amber
3. The outline is invisible to Veyon's screen capture (`WDA_EXCLUDEFROMCAPTURE`)
4. When the master disconnects, the outline disappears
5. Uncheck `Show Amber Outline` or `Show Red Outline` in the tray menu and
   verify each warning can be hidden independently

### 3. Window Filtering

1. Add a keyword to `config/blacklist.txt` (e.g., `Firefox`)
2. Open a window matching that keyword
3. The master keeps seeing the previous clean pixels in that window's area,
   so the blacklisted app stays hidden instead of turning into a black box
4. The blacklist file is hot-reloaded (edit while running)

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| "Failed to write registry" | Not running as admin | Run elevated |
| "Failed to bind port" | Port already in use | Ensure old UltraVNC moved to new port; wait for service restart |
| "Failed to connect to upstream VNC" | UltraVNC not ready | Wait a few seconds, it should reconnect automatically |
| Master can't see student | Service restart failed | Manually `net start VeyonService` |

## Important Note

This repository is edited from Linux. The code is architecture-correct but must
be built and tested on a real Windows machine with Veyon installed.
