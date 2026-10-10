---
name: build-and-run
description: Build, test, package, run and screenshot 26Viewer (CMake presets, MSYS2 UCRT64 GCC + Qt 6). Use whenever compiling, running tests, launching the GUI/CLI, verifying a UI change visually, or producing dist/.
---

# Build and run 26Viewer

Toolchain: MSYS2 UCRT64 at `C:\msys64` (GCC 15, CMake 4, Ninja, Qt 6.10, Catch2 3, zlib).
There is no MSVC on this machine. Presets live in `CMakePresets.json`.

## Commands (Bash tool)

Always prefix the MSYS2 bin dir, from the repo root:

```bash
cd "/f/project 2/26Viewer" && export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --preset debug                       # configure (re-run after adding files/CMake changes)
cmake --build --preset debug 2>&1 | grep -E "error|warning:" -A3   # build; empty output = clean
ctest --preset debug                       # unit tests
BLUR_DIR="F:/Games/BLUR" ctest --preset debug   # + real-archive test
```

Release + self-contained package:

```bash
cmake --preset release && cmake --build --preset release && cmake --install build/release
# -> dist/release/26Viewer.exe (+ DLLs, plugins). Verify it runs without MSYS2:
PATH="/c/Windows/System32:/c/Windows" /usr/bin/timeout 60 dist/release/26viewer-cli.exe games
```

Note: with a restricted PATH, call `/usr/bin/timeout` explicitly (Windows has its own `timeout.exe`).

## CLI checks (fast feedback for format work)

```bash
build/debug/bin/26viewer-cli.exe info   "F:/Games/BLUR/cache/root.pak"
build/debug/bin/26viewer-cli.exe verify "F:/Games/BLUR"          # decompress everything
build/debug/bin/26viewer-cli.exe extract "F:/Games/BLUR/cache/root.pak" <scratch>/out graphics/textures
```

Extract into the session scratchpad, never into the repo.

## Seeing the GUI (no desktop control needed)

The app renders off-screen and saves a PNG, then exits:

```bash
timeout 60 build/debug/bin/26Viewer.exe "F:/Games/BLUR" \
  --select "cache/root.pak/graphics/textures/flags/amboyflag.texture" \
  --tab preview --theme dark --screenshot "$(cygpath -w <scratch>/shot.png)"
```

Then Read the PNG. Check both `--theme light` and `--theme dark` after any UI change.
Add `--edit-mode` to capture Edit Mode UI (badge, "not backed up" banner) without the
confirmation dialog. Without a path, `--screenshot` captures start-up (reopens the last session).
Screenshot runs write the `[session]` keys of the user's real settings INI
(`%APPDATA%\26Viewer\26Viewer.ini`); for edit/backup experiments use archive *copies* in the scratchpad.
`--tab` accepts preview|structure|hex|properties. `--select` takes a virtual path
(`<folder>/<archive>/<path inside archive>`).

## Rules

- Zero warnings is the bar; fix them, don't silence them.
- After adding a source file, add it to the target's `CMakeLists.txt` (sources are listed explicitly).
- Don't write to `dist/` or `build/` by hand; they are generated and git-ignored.
