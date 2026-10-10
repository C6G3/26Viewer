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
   - [ ] Confirm the CI workflow (`.github/workflows/build.yml`) passes on GitHub Actions
3. [ ] Windows resources: `.ico` app icon + VERSIONINFO via `app.rc`
4. [ ] Recent games / recent archives menu (File ▸ Recent)
5. [ ] Status-bar breadcrumb and "Go to path…" (`Ctrl+L`) using `MainWindow::selectPath`
6. [ ] Hex view: selection, copy as hex/C array, go-to-offset, find bytes
7. [ ] Image view: channel toggles (R/G/B/A), mip selector, export as PNG
8. [ ] Export converted formats: `.texture` → `.dds`/`.png` on extraction (option)
9. [ ] Parallel extraction (several files at once; archives already support concurrent reads)
10. [ ] Settings dialog (default extract folder, preview size limits)
11. [ ] Release workflow: tag → CI builds `dist/release` → zip attached to a GitHub Release

## Phase 4 — Deeper Blur support

1. [ ] `ARCH` `.levelstream` archives (loose in `levels/`) → new `ArchiveFormat`
2. [ ] `.model` geometry: decode `MdlDat`/`Render` chunks (vertex/index buffers)
3. [ ] 3D preview tab: Qt RHI (`QRhiWidget`, Qt 6.7+) — orbit camera, textured meshes
4. [ ] `.baf` `BANK` audio → WAV export and playback (Qt Multimedia)
5. [ ] Data tables (`.bin`, `.loc`, `.xt`) → table view
6. [ ] `.bpr` (`2RZB`) UI packs, `.ipf` layouts, `.pf` fonts
7. [ ] Identify the PAK `hash`/digest/name-hash algorithms (enables repacking)

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
