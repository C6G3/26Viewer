# Building

## Prerequisites (Windows)

1. Install [MSYS2](https://www.msys2.org/) to `C:\msys64` (the default).
2. In an **MSYS2 UCRT64** shell, install the toolchain and libraries:

   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-catch
   ```

Tested with GCC 15.2, CMake 4.2, Qt 6.10.1. Minimums: C++23 compiler
(`std::expected`, `std::print`), CMake 3.25, Qt 6.8.

If MSYS2 lives elsewhere, edit the `mingw-base` preset in `CMakePresets.json`
(or create a git-ignored `CMakeUserPresets.json` that overrides it).

## Build, test, run

The presets put `C:\msys64\ucrt64\bin` on `PATH` for you, so these work from
PowerShell, cmd or Git Bash:

```bash
cmake --preset debug            # configure → build/debug
cmake --build --preset debug    # compile
ctest --preset debug            # unit tests
```

Binaries land in `build/<preset>/bin/`. When launching them directly (outside
the presets), `C:\msys64\ucrt64\bin` must be on `PATH` so the Qt DLLs are found.

Integration test against a real install (optional):

```bash
BLUR_DIR="F:/Games/BLUR" ctest --preset debug
```

### Options

| CMake option | Default | Meaning |
|---|---|---|
| `VIEWER_GAMES` | `blur` | Game modules to compile in (`;`-separated). |
| `VIEWER_BUILD_APP` | `ON` | Qt desktop application. |
| `VIEWER_BUILD_CLI` | `ON` | `26viewer-cli`. |
| `VIEWER_BUILD_TESTS` | `ON` | Catch2 tests. |

## Packaging (dist/)

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release
```

`dist/release/` is then self-contained: `cmake/Deploy.cmake` runs
`windeployqt6` and copies every MinGW/MSYS2 DLL the binaries need (resolved
with `file(GET_RUNTIME_DEPENDENCIES)`). Zip that folder to ship it.

## Making a release

GitHub builds releases (`.github/workflows/build.yml`):

1. Set the new version in `CMakeLists.txt` (`project(26Viewer VERSION 0.3.0 …)`), commit and push.
2. Tag that commit and push the tag:

   ```bash
   git tag v0.3.0
   git push origin v0.3.0
   ```

3. The workflow builds, runs the tests, packages `dist/`, checks that the tag
   matches the version, and publishes **26Viewer v0.3.0** on the repository's
   Releases page with `26Viewer-v0.3.0-windows-x64.zip` and notes listing the
   changes since the previous release. If anything fails, nothing is published:
   fix it, delete the tag (`git push --delete origin v0.3.0`, `git tag -d v0.3.0`)
   and tag again.

## UI screenshots from the command line

```bash
build/release/bin/26Viewer.exe "F:/Games/BLUR" --select "cache/root.pak/graphics/textures/flags/amboyflag.texture" --theme dark --screenshot docs/images/screenshot-dark.png
```

The window is rendered off-screen, so nothing flashes on the desktop.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `undefined reference to std::__open_terminal` | Link `stdc++exp` (already done in `cmake/ProjectOptions.cmake`). |
| App starts then exits silently | Qt DLLs not found: add `C:\msys64\ucrt64\bin` to `PATH` or use `dist/`. |
| `Could not find a package configuration file provided by "Qt6"` | Install `qt6-base` or point `CMAKE_PREFIX_PATH` at your Qt. |
