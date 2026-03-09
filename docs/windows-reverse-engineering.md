# Veyon Reverse Engineering Notes for Windows

This document records the Veyon source evidence that drives the Windows-native Veyoff design. It exists to prevent hand-wavy API choices and to keep implementation decisions tied to the actual upstream architecture.

## Capture Pipeline

The strongest evidence for Veyon's Windows capture path is in `veyon/plugins/vncserver/ultravnc-builtin/CMakeLists.txt`. That file builds Veyon's Windows VNC backend around bundled UltraVNC sources, including:

- `DeskdupEngine.cpp`
- `ScreenCapture.cpp`
- `HideDesktop.cpp`
- `vncdesktop.cpp`
- `vncDesktopSW.cpp`

It also defines `_USE_DESKTOPDUPLICATION`, which confirms that Veyon's preferred Windows capture path supports DXGI Desktop Duplication when available.

`veyon/plugins/vncserver/ultravnc-builtin/BuiltinUltraVncServer.cpp` and `veyon/plugins/vncserver/ultravnc-builtin/UltraVncConfigurationWidget.ui` expose the capture-related knobs Veyon considers important on Windows:

- layered/semi-transparent window capture
- full-screen polling fallback
- desktop duplication engine toggle
- multi-monitor support
- CPU cap / turbo mode

That tells us two important things:

1. Veyon treats Windows capture as backend-configurable, not as a single hardcoded mechanism.
2. Desktop Duplication is preferred, but polling fallback still matters.

For Veyoff, the initial Windows implementation will use a reliable Win32/GDI path with layered-window capture enabled via `CAPTUREBLT`, while the architecture keeps room for a future DXGI backend.

## Session and Port Model

`veyon/server/src/ComputerControlServer.cpp` shows that Veyon accepts remote sessions through a proxy server bound to `veyonServerPort + sessionId`, then forwards to the internal VNC server.

`veyon/server/src/VncServer.cpp` shows the internal VNC server port is `vncServerPort + sessionId`.

`veyon/core/src/VeyonConfigurationProperties.h` defines the default base ports used by Veyon:

- external Veyon control server: `11100`
- internal VNC server: `11200`

`veyon/plugins/platform/windows/WindowsSessionFunctions.cpp` maps the console session to logical session `0` and non-console sessions to their actual Windows session IDs. That means the real remote-access ports on Windows are session-aware, not just fixed `11100` and `11200` in all cases.

Veyoff therefore watches these port families:

- `5900` as a generic VNC compatibility fallback
- `11100 + currentSessionId`
- `11200 + currentSessionId`
- base `11100` / `11200` as console fallbacks

This is much closer to Veyon's behavior than a generic "is port 5900 open" check.

## Windows Session Supervision

`veyon/plugins/platform/windows/WtsSessionManager.cpp` and `veyon/plugins/platform/windows/WindowsServiceCore.cpp` show that Veyon is deeply session-aware on Windows.

Important details:

- Veyon queries active console and WTS sessions.
- It distinguishes console vs. RDP sessions.
- The service supervisor reacts to session changes and starts/stops per-session server processes.

Veyoff does not need to reproduce the full service/process orchestration just to provide local privacy controls, but it does need to respect session-scoped port selection and user-session visibility.

## Overlay and User Signaling

I did not find a built-in student-facing "master is viewing you" overlay in Veyon. What I found instead:

- `veyon/server/src/ComputerControlServer.cpp` shows tray notifications for remote access.
- `veyon/core/src/LockWidget.cpp` shows how Veyon builds an always-on-top fullscreen lock surface.
- `veyon/plugins/screenlock/ScreenLockFeaturePlugin.cpp` and demo-mode code reuse that lock-style fullscreen surface.

So the exact overlay idea is not copied from Veyon. The closest precedent is Veyon's lock/demo presentation model and tray-based remote-access messaging. Veyoff intentionally adds a more explicit topmost banner overlay because that matches the user goal better.

## Keyboard Strategy

`veyon/plugins/platform/windows/WindowsKeyboardShortcutTrapper.cpp` uses `SetWindowsHookEx(WH_KEYBOARD_LL, ...)` to trap disruptive shortcuts like `Alt+Tab`, `Alt+F4`, and the Windows key during remote control.

That is useful evidence, but it is not the right tool for a simple freeze toggle. For Veyoff's single global shortcut, `RegisterHotKey` is the better Windows API because:

- it is narrower in scope
- it is easier to reason about
- it avoids writing a global low-level hook when not necessary

This is an intentional divergence from Veyon's remote-control shortcut trapper.

## Window Interference and Desktop Surfaces

`veyon/plugins/platform/windows/WindowsSessionFunctions.cpp` contains Veyon's "interfering window" inspection logic. It enumerates desktop windows, checks layered/topmost/transparent styles, and can fix or terminate those windows. That is valuable precedent for Veyoff's selective hiding because it confirms that window-style-aware desktop inspection is already part of Veyon's Windows platform model.

For Veyoff, the filtering strategy is:

- enumerate visible top-level windows
- obtain desktop bounds using DWM extended frame bounds where possible
- match titles against blacklist entries
- paint black rectangles over those bounds in the captured frame

## Implementation Implications

The reverse-engineered Veyon guidance that directly shapes Veyoff is:

1. Use session-aware Veyon port families, not only raw VNC defaults.
2. Keep capture backend pluggable because Veyon clearly does.
3. Prefer a native Windows message loop and Win32 APIs for the hotkey and overlay.
4. Use desktop/window inspection informed by DWM/Win32 geometry instead of app-specific hacks.
5. Do not claim Veyon has a student-facing viewing overlay when the source only clearly shows tray notifications and fullscreen lock/demo surfaces.
