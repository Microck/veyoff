# Veyoff: Privacy-Enhanced Screen Monitoring Client

## CRITICAL: Autonomous Overnight Execution Mode

You are running in Ralph loop mode. This prompt will repeat until ALL THREE features are working. Do NOT stop after Phase 1. Continue through all phases until the final completion promise is triggered.

## Goal

Build a working client-side program with THREE functional features:
1. Screen freeze hotkey (Ctrl+Shift+F)
2. Master presence overlay
3. Selective window hiding

## Iteration Protocol (FOLLOW EVERY LOOP)

### Step 1: Check Current State
Run these commands FIRST every iteration:
```bash
# Check what exists
ls -la
ls -R docs/ 2>/dev/null || echo "No docs yet"
ls -R src/ 2>/dev/null || echo "No src yet"
find . -name "*.py" -o -name "*.cpp" -o -name "*.c" 2>/dev/null | head -20
git log --oneline -5 2>/dev/null || echo "No commits yet"

# Check if Veyon source exists
ls -la veyon/ 2>/dev/null || ls -la opensrc/veyon/ 2>/dev/null || echo "Veyon not cloned yet"
```

### Step 2: Read Progress Tracker
Read `docs/PROGRESS.md` to see what's been completed. If it doesn't exist, you're on iteration 1.

### Step 3: Do ONE Concrete Task
Pick the NEXT incomplete task from the phase list below. Do it. Verify it worked.

### Step 4: Update Progress Tracker
Update `docs/PROGRESS.md` with what you just completed and what's next.

### Step 5: Verify Your Work
Run verification commands for what you just built. If tests fail, fix them before moving on.

### Step 6: Check for Completion
If ALL THREE features work (verified by running them), output the completion promise.

## Phase Breakdown (Complete ALL Phases)

### Phase 1: Research & Architecture
**Tasks:**
1. Clone Veyon: `opensrc veyon/veyon` or `git clone https://github.com/veyon/veyon.git`
2. Read key files:
   - `veyon/core/src/VncServerClient.cpp` (screen capture)
   - `veyon/core/src/ScreenFrameBuffer.cpp` (frame buffer)
   - `veyon/core/src/VncConnection.cpp` (network protocol)
3. Create `docs/veyon-analysis.md` with findings (min 500 words)
4. Create `docs/architecture.md` with Veyoff design (min 300 words)
5. Choose tech stack (recommend Python + pynput + mss + python-xlib for rapid prototyping)
6. Initialize project structure:
   ```
   src/
   src/veyoff/
   src/veyoff/__init__.py
   src/veyoff/capture.py
   src/veyoff/hotkey.py
   src/veyoff/overlay.py
   src/veyoff/filter.py
   tests/
   requirements.txt
   setup.py
   ```

**Verification:**
```bash
test -f docs/veyon-analysis.md && echo "✓ Analysis exists"
test -f docs/architecture.md && echo "✓ Architecture exists"
test -f src/veyoff/__init__.py && echo "✓ Project initialized"
test -f requirements.txt && echo "✓ Dependencies defined"
```

**Completion Signal:** When all verification passes, update `docs/PROGRESS.md` with "Phase 1 Complete" and move to Phase 2.

### Phase 2: Screen Capture Foundation
**Tasks:**
1. Install dependencies: `pip install mss pillow pynput python-xlib`
2. Create `src/veyoff/capture.py` with:
   - Function to capture screen using mss
   - Function to save frame to buffer
   - Function to return cached frame (for freeze feature)
3. Create `tests/test_capture.py` with basic capture test
4. Create `src/veyoff/main.py` that captures screen every 100ms and saves to /tmp/veyoff_test.png

**Verification:**
```bash
cd /home/ubuntu/workspace/veyoff
python -m pytest tests/test_capture.py -v
python src/veyoff/main.py &
sleep 2
test -f /tmp/veyoff_test.png && echo "✓ Capture works"
pkill -f "python src/veyoff/main.py"
```

**Completion Signal:** When verification passes, update progress and move to Phase 3.

### Phase 3: Hotkey System (FEATURE 1)
**Tasks:**
1. Create `src/veyoff/hotkey.py` with:
   - Global hotkey listener using pynput
   - Freeze/unfreeze toggle state
   - Integration with capture.py to return cached frame when frozen
2. Update `src/veyoff/main.py` to:
   - Start hotkey listener
   - Show "FROZEN" indicator in terminal when frozen
   - Return cached frame when frozen, live frame when not
3. Create `tests/test_hotkey.py` with hotkey registration test
4. Create `docs/FEATURE_1_DEMO.md` with instructions to test

**Verification:**
```bash
cd /home/ubuntu/workspace/veyoff
python -m pytest tests/test_hotkey.py -v
# Manual test instructions in FEATURE_1_DEMO.md
echo "✓ Hotkey system implemented"
```

**Completion Signal:** When tests pass and demo instructions exist, update progress and move to Phase 4.

### Phase 4: Master Detection & Overlay (FEATURE 2)
**Tasks:**
1. Create `src/veyoff/overlay.py` with:
   - Function to detect VNC/Veyon connections (check netstat for port 5900 or Veyon ports)
   - Function to draw overlay using tkinter or pygame
   - Overlay shows: "👁️ MASTER VIEWING" in red border
2. Update `src/veyoff/main.py` to:
   - Monitor for master connections every 1 second
   - Show/hide overlay based on connection state
3. Create `tests/test_overlay.py` with overlay rendering test
4. Create `docs/FEATURE_2_DEMO.md` with test instructions

**Verification:**
```bash
cd /home/ubuntu/workspace/veyoff
python -m pytest tests/test_overlay.py -v
# Simulate VNC connection: nc -l 5900 &
# Run main.py and verify overlay appears
echo "✓ Overlay system implemented"
```

**Completion Signal:** When tests pass and demo works, update progress and move to Phase 5.

### Phase 5: Selective Window Hiding (FEATURE 3)
**Tasks:**
1. Create `src/veyoff/filter.py` with:
   - Function to enumerate windows using python-xlib
   - Function to check if window title matches blacklist
   - Function to composite screen capture with blacklisted windows removed/blacked out
2. Create `config/blacklist.txt` with example entries:
   ```
   Firefox
   Chrome
   Signal
   ```
3. Update `src/veyoff/capture.py` to:
   - Apply window filter before returning frame
   - Replace blacklisted windows with black rectangles or desktop background
4. Create `tests/test_filter.py` with window filtering test
5. Create `docs/FEATURE_3_DEMO.md` with test instructions

**Verification:**
```bash
cd /home/ubuntu/workspace/veyoff
python -m pytest tests/test_filter.py -v
# Open Firefox, add to blacklist, verify it's hidden in capture
echo "✓ Window filtering implemented"
```

**Completion Signal:** When tests pass and demo works, update progress and move to Phase 6.

### Phase 6: Integration & Final Testing
**Tasks:**
1. Create `src/veyoff/daemon.py` that runs all three features together:
   - Screen capture with freeze support
   - Hotkey listener
   - Master detection with overlay
   - Window filtering
2. Create `install.sh` script to set up dependencies
3. Create `run.sh` script to start Veyoff daemon
4. Create `docs/USER_GUIDE.md` with:
   - Installation instructions
   - How to use each feature
   - Configuration options
   - Troubleshooting
5. Test all three features working together:
   - Start daemon
   - Press hotkey to freeze
   - Simulate master connection
   - Add window to blacklist
   - Verify all work simultaneously

**Verification:**
```bash
cd /home/ubuntu/workspace/veyoff
bash install.sh
bash run.sh &
sleep 3
# All three features should be active
ps aux | grep veyoff
echo "✓ Integration complete"
```

**Completion Signal:** When all features work together, output final promise.

## Progress Tracking

After EVERY iteration, update `docs/PROGRESS.md` with this format:

```markdown
# Veyoff Development Progress

## Current Phase: [Phase Number and Name]

## Completed Tasks
- [x] Task 1
- [x] Task 2

## Current Task
- [ ] Task 3 (IN PROGRESS)

## Next Tasks
- [ ] Task 4
- [ ] Task 5

## Blockers
[Any issues encountered]

## Last Iteration Summary
[What you just did this iteration]

## Next Iteration Plan
[What you'll do next iteration]
```

## Error Recovery

If you encounter errors:
1. Read the error message carefully
2. Check if dependencies are installed
3. Try a simpler approach
4. Document the issue in `docs/PROGRESS.md`
5. Try an alternative implementation
6. DO NOT give up - try at least 3 different approaches

## Verification Commands (Run Every Iteration)

```bash
# Project structure check
ls -R | head -50

# Code quality check
find src/ -name "*.py" -exec python -m py_compile {} \; 2>&1 | head -20

# Test suite
python -m pytest tests/ -v 2>&1 | tail -30

# Git status
git status --short

# Progress check
cat docs/PROGRESS.md 2>/dev/null | tail -20
```

## Final Completion Promise

Output this ONLY when ALL THREE features are working and verified:

<promise>VEYOFF ALL FEATURES COMPLETE</promise>

Criteria for completion:
1. ✓ Screen freeze hotkey works (can press Ctrl+Shift+F and screen freezes)
2. ✓ Master overlay appears when VNC connection detected
3. ✓ Window filtering works (blacklisted windows hidden from capture)
4. ✓ All three features work together without crashes
5. ✓ User guide exists with clear instructions
6. ✓ Tests pass for all features

## Constraints

- Must not require root/admin privileges
- Should work on Linux (X11 initially, Wayland if time permits)
- Must be undetectable by master under normal operation
- Performance impact < 5% CPU overhead
- Code must be readable and maintainable

## Tech Stack Decision

**RECOMMENDED:** Python-based implementation for rapid development
- **Screen capture:** mss (fast, cross-platform)
- **Hotkeys:** pynput (global hotkey support)
- **Overlay:** tkinter or pygame (simple, built-in)
- **Window enumeration:** python-xlib (X11 window access)
- **VNC detection:** psutil + socket (connection monitoring)

Alternative: C++/Qt (if Python approach fails after 10 iterations)

## Notes

- Focus on getting working prototypes quickly
- Optimize later
- Document as you go
- Test each feature independently before integration
- Keep code modular
- This is for educational/privacy purposes - document ethical considerations in USER_GUIDE.md
