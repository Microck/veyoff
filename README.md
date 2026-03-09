<picture>
  <img alt="veyoff" src="https://litter.catbox.moe/wf3ckinihcfsmuet.svg" width="240">
</picture>

**an RFB man-in-the-middle proxy that controls what the Veyon master sees on a student machine.**

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/c%2B%2B-20-blue.svg)]()
[![Windows](https://img.shields.io/badge/platform-windows%2010%2B-0078D6.svg)]()

veyoff sits between Veyon's internal proxy and the UltraVNC screen capture engine on the local machine. it forwards framebuffer data transparently in normal mode, and intercepts it when freeze or window hiding is active. the teacher sees nothing unusual — just a brief 1-2s connection blip during setup.

---

## how it works

```
veyon master (teacher)
    │
    │  connects to port 11100+session (veyon custom auth)
    ▼
veyon VncProxyServer (part of veyon service, untouched)
    │
    │  connects to localhost:11200+session (standard RFB)
    ▼
VEYOFF PROXY (binds port 11200+session, the original VNC port)
    │   transparent forwarding in normal mode
    │   intercepts framebuffer when frozen / blacklisting
    ▼
real UltraVNC (port 11250+session, redirected via registry)
```

**setup is fully automated.** on launch, veyoff reads the VNC port from the veyon registry, redirects it +50, restarts the veyon service, and binds the proxy on the original port. on exit, it restores everything.

---

## features

1. **screen freeze** (`Ctrl+Shift+F`) — master sees a frozen frame while you keep working normally.

2. **master presence overlay** — red banner warning when a master is actively connected. the banner is invisible to veyon's screen capture (rendered in a layered window excluded from DWM capture).

3. **selective window hiding** — windows matching keywords in `config/blacklist.txt` are blacked out from the master's view. add one keyword per line:
   ```
   Firefox
   Chrome
   Signal
   ```

---

## quickstart

### build (visual studio 2022 + cmake)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

or with ninja:

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build --config Release
```

### run (as administrator)

```powershell
.\build\Release\veyoff-windows.exe
```

veyoff automatically:
- reads veyon's config from `HKLM\SOFTWARE\Veyon Solutions\Veyon\Network`
- redirects the internal VNC port (11200 → 11250)
- restarts `VeyonService` (brief 1-2s blip)
- starts the RFB proxy and overlay

on exit (`Ctrl+C`), it restores the original port and restarts the service.

### custom blacklist

```powershell
.\build\Release\veyoff-windows.exe --blacklist C:\path\to\blacklist.txt
```

---

## how the proxy works

the proxy speaks RFB (VNC protocol) at the message level. it doesn't just relay bytes — it parses every RFB message to know where message boundaries are, which lets it:

- **freeze**: cache the last full framebuffer update from upstream and replay it to the master on every `FramebufferUpdateRequest`, while the real screen keeps changing
- **blacklist**: capture the real screen via GDI, black out regions occupied by blacklisted windows (matched by title substring), encode the result as a raw RFB `FramebufferUpdate`, and send that instead
- **SetEncodings rewrite**: forces the upstream VNC server to use only `Raw` encoding, so veyoff never needs to decode compressed pixel data

the master's VNC viewer has no way to tell the difference — it receives valid RFB frames either way.

---

## requirements

- windows 10+
- administrator privileges (registry + service control)
- veyon installed and running
- visual studio 2022 or mingw-w64 with c++20 support
- cmake 3.21+

---

## docs

- [architecture](docs/windows-architecture.md) — proxy design, RFB protocol handling, overlay mechanics
- [build & validation](docs/windows-build.md) — build instructions, manual testing procedures
- [veyon reverse engineering](docs/windows-reverse-engineering.md) — source analysis of veyon internals

---

## project layout

```
veyoff/
├── src/windows/
│   └── veyoff-windows.cpp    # single-file implementation (~1500 lines)
├── config/
│   └── blacklist.txt          # window title keywords to hide (one per line)
├── toolchains/
│   └── windows-mingw64.cmake  # cross-compilation toolchain (linux → windows)
├── docs/
│   ├── windows-architecture.md
│   ├── windows-build.md
│   └── windows-reverse-engineering.md
└── CMakeLists.txt
```

---

## ethical notice

this tool is for legitimate privacy needs — entering passwords, personal communications during monitored sessions. comply with your organization's policies and applicable laws.

---

## license

MIT. see [LICENSE](LICENSE).
