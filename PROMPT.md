# Veyoff: Privacy-Enhanced Screen Monitoring Client

## Goal

Build a client-side program that works with Veyon-style screen monitoring systems and provides privacy controls for the monitored user (non-master).

## Target Features

### 1. Screen Freeze Hotkey
- Implement a global hotkey (e.g., Ctrl+Shift+F) that "freezes" the screen from the master's perspective
- When frozen, the master sees a static snapshot while the user continues working normally
- Pressing the hotkey again unfreezes and resumes live streaming
- Must work seamlessly without alerting the master

### 2. Master Presence Overlay
- Display a visual overlay (border, icon, or notification) when the master is actively viewing or has gone "fullscreen" into this screen
- Overlay must be invisible to the master's view
- Should be subtle but noticeable to the user
- Include visual indicator of master's current viewing mode

### 3. Selective Window Hiding
- Allow user to mark specific programs/windows as "hidden from master"
- Hidden windows should not appear in the screen stream sent to the master
- Implement a whitelist/blacklist system for window titles or process names
- Must handle window switching and overlapping gracefully

## Implementation Strategy

### Phase 1: Research & Architecture (Current Phase)
1. Clone and analyze Veyon source code: https://github.com/veyon/veyon
2. Identify key components:
   - Screen capture mechanism
   - Network protocol for master-client communication
   - VNC/RFB implementation details
   - Client-side hooks and capture pipeline
3. Document architecture in `docs/veyon-analysis.md`
4. Design Veyoff architecture in `docs/architecture.md`
5. Choose tech stack (likely C++/Qt to match Veyon, or Python for rapid prototyping)

### Phase 2: Screen Capture Interception
1. Implement screen capture hook/wrapper
2. Build frame buffer manipulation layer
3. Create freeze mechanism (cache last frame)
4. Test with dummy VNC server

### Phase 3: Hotkey System
1. Implement global hotkey registration
2. Wire hotkey to freeze/unfreeze logic
3. Add visual feedback for user
4. Test across window managers

### Phase 4: Master Detection & Overlay
1. Monitor incoming master connections
2. Detect fullscreen/viewing mode changes
3. Implement overlay rendering (separate layer)
4. Ensure overlay is excluded from capture pipeline

### Phase 5: Selective Window Hiding
1. Enumerate windows and processes
2. Implement window filtering in capture pipeline
3. Build configuration UI for whitelist/blacklist
4. Handle edge cases (overlapping windows, transparency)

### Phase 6: Integration & Testing
1. Test against real Veyon master
2. Performance optimization
3. Security hardening
4. Documentation

## Success Criteria

- [ ] Veyon codebase analyzed and documented
- [ ] Architecture document created
- [ ] Project structure initialized with build system
- [ ] Screen freeze hotkey works without master detection
- [ ] Overlay displays when master connects
- [ ] At least one window can be hidden from master view
- [ ] All features work together without crashes
- [ ] Performance impact < 5% CPU overhead
- [ ] No obvious detection by master

## Completion Promise

When all Phase 1 tasks are complete and you have a working prototype of at least one feature, output:

<promise>VEYOFF PHASE 1 COMPLETE</promise>

## Current Iteration Focus

Start with Phase 1: Research & Architecture. Your immediate tasks:

1. Use `opensrc` or clone Veyon repository
2. Read key source files (focus on screen capture, VNC server, client components)
3. Document findings in `docs/veyon-analysis.md`
4. Propose initial architecture in `docs/architecture.md`
5. Initialize project structure with appropriate build system
6. Create a minimal "hello world" that can capture screen

## Notes

- Prioritize understanding Veyon's screen capture pipeline first
- This is for educational/privacy purposes - document ethical considerations
- Focus on Linux initially (X11/Wayland), expand to Windows/macOS later
- Keep code modular for easy iteration
- Write tests as you go

## Constraints

- Must not require root/admin privileges for basic features
- Should work with unmodified Veyon master
- Must be undetectable by master under normal operation
- Performance must not degrade user experience
