# Roadmap

A step-by-step plan for growing 26Viewer from a Blur archive browser into a
multi-game asset viewer. Each phase leaves the app shippable. Tick boxes as you go.

---

## Phase 0 — Foundations ✅

- [x] Repository layout: `src/{core,games,cli,app}`, `tests/`, `docs/`, `tools/`, `cmake/`, git-ignored `build/` + `dist/`
- [x] CMake 3.25+ with presets (`debug`, `release`), C++23, strict warnings
- [x] Build-time game selection (`VIEWER_GAMES` → generated `registerBuiltinGames()`)
- [x] Core library without Qt: `Result<T>`, `BinaryReader`, `RandomAccessFile`, compression, `Archive`/`ArchiveFormat`, `ContentRegistry`, `Game`/`GameRegistry`, `Vfs`
- [x] Catch2 tests (synthetic archives, no game data needed)

## Phase 1 — Blur archives ✅

- [x] Reverse-engineer 2KAP v1/v2 (name XOR, 32/40/64-byte entries, stored/zlib/chunked)
- [x] Verified on all 6,070 packed files; byte-compared against a QuickBMS dump
- [x] Format documented in `docs/formats/blur.md`, Python reference in `tools/research/`

## Phase 2 — Desktop app MVP ✅

- [x] Game picker with install detection and remembered paths
- [x] Tree of install folder + archives, sorting, debounced recursive filter
- [x] Preview tabs: image, text, structure, hex, properties (background loading)
- [x] Extraction with progress/cancel, path sanitising
- [x] Light/Dark/System theme (live OS following), Windows 11 style, SVG type icons
- [x] CLI: `games`, `info`, `list`, `verify`, `extract`
- [x] Self-contained `dist/` via `cmake --install`

## Phase 3 — Polish & GitHub release (next)

1. [x] Choose a license, add `LICENSE` (MIT)
2. [x] `git init`, first commit, push to https://github.com/C6G3/26Viewer
   - [x] Confirm the CI workflow (`.github/workflows/build.yml`) passes on GitHub Actions
3. [x] Windows resources: `.ico` app icon (rendered from SVG at build time by `tools/icongen`) + VERSIONINFO
4. [x] Reopen the last game + selection at start-up (settings in `%APPDATA%\26Viewer\26Viewer.ini`)
   - [ ] Recent games / recent archives menu (File ▸ Recent)
4a. [x] OpenIV-style browsing: folder/archive tree → contents list → double-click to preview,
   Backspace back, clickable path bar, search within the open folder, Properties (`Alt+Enter`)
   - [x] Previews open in their own windows (toolbar for per-file tools, drop-to-replace)
   - [x] Game's .exe icon on the tree's top node; optional files in the tree
   - [x] Open archives / any folder alongside the game (extra top-level entries, remembered per game)
   - [ ] Per-file tools in the preview window: image channels, mip selector, export PNG
4b. [x] Replace assets in place (dialog, toolbar, drag & drop), Restore Original
   - [x] Edit Mode toggle (OpenIV-style, confirmation, status badge) gating all writes
   - [x] Explicit backups: red "not backed up" banner + Back Up Now; no automatic backups
   - [ ] Test replaced Blur files in-game (normal archives, signed gamedata archives)
   - [ ] Batch replace: drop a folder whose layout mirrors the archive
5. [ ] Status-bar breadcrumb and "Go to path…" (`Ctrl+L`) using `MainWindow::selectPath`
6. [ ] Hex view: selection, copy as hex/C array, go-to-offset, find bytes
7. [ ] Image view: channel toggles (R/G/B/A), mip selector, export as PNG
8. [ ] Export converted formats: `.texture` → `.dds`/`.png` on extraction (option)
9. [ ] Parallel extraction (several files at once; archives already support concurrent reads)
10. [ ] Settings dialog (default extract folder, preview size limits)
11. [ ] Release workflow: tag → CI builds `dist/release` → zip attached to a GitHub Release

## Phase 4 — Deeper Blur support

1. [ ] `ARCH` `.levelstream` archives (loose in `levels/`) → new `ArchiveFormat`
2. [x] `.model` geometry: vertex/index buffers, all LODs, shadow hulls (see docs/formats/blur.md); next: part names, materials, UVs
3. [x] 3D preview: orbit camera, parts tree, solid/wireframe/grid/shadow, LOD picker (OpenGL); next: textures and game-accurate shading
4. [x] `.baf` `BANK` audio → WAV export and playback (Qt Multimedia); next: import WAVs / replace sounds
4b. [x] Bink `.bik` video playback (Qt Multimedia FFmpeg backend), audio track picker
4c. [x] Text viewer/editor: colour coding (XML, INI, CSV, JSON, Lua, shaders, QuickBMS), encodings round-trip, saving through Edit Mode
5. [ ] Data tables (`.bin`, `.loc`, `.xt`) → table view
6. [ ] `.bpr` (`2RZB`) UI packs, `.ipf` layouts, `.pf` fonts
7. [x] PAK hashes (recovered from Blur.exe, see docs/formats/blur.md): digest = SHA-1(stored),
   content hash = Σ ~CRC-32 per 32 KiB block, name hash = ~CRC-32 of the lower-case `\path`
8. [ ] Add new files to archives (full repack: grow name table + file table, keep entries sorted by name hash)

## Phase 5 — Multi-game platform

1. [ ] Promote the Bizarre chunk format to `src/games/shared/bizarre/` when a second Bizarre game arrives
2. [ ] Add a second game module (proves the abstractions; adjust interfaces where they leak)
3. [ ] Global search across all archives (name + extension + category filters)
4. [ ] "Merged view": overlay all archives by virtual path (how the game sees them)
5. [ ] Optional runtime plugins (DLL game modules loaded from `plugins/`) behind the same `Game` interface
6. [ ] Localisation via Qt Linguist (`tr()` is already used throughout)

## Phase 6 — Modding (stretch)

1. [ ] Archive writing (repack 2KAP) once hashes are understood
2. [ ] Replace-file workflow with backups
3. [ ] Diff two installs / two archive versions

---

### Modern C++ / UI techniques used (and where)

| Technique | Where |
|---|---|
| `std::expected` error handling | `core/Error.hpp`, everywhere |
| `std::span`, `std::byte`, ranges, `std::format`/`std::print` | core, CLI |
| Positional, lock-free concurrent file reads | `core/RandomAccessFile.cpp` |
| Interfaces + registries instead of `if (game == …)` | `ContentRegistry`, `GameRegistry` |
| Code generation for module registration | `cmake/Games.cmake` |
| `QtConcurrent::run` + `QFutureWatcher`/`QPromise` (progress, cancel) | `MainWindow`, `PreviewPane`, `Extractor` |
| Generation counters to drop stale async results | `PreviewPane` |
| `QStyleHints::setColorScheme` / `colorSchemeChanged` (live dark mode) | `app/Theme.cpp` |
| Palette-driven custom painting | `HexView`, `ImageCanvas` |
| Fluent theme icons (`QIcon::ThemeIcon`) + SVG resources | `app/Icons.cpp` |
| Off-screen rendering for scripted screenshots | `app/main.cpp --screenshot` |
