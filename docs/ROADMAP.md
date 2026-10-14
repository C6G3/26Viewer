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
   - [x] Recent games / archives / folders menu (File ▸ Open Recent, ten entries, missing ones greyed)
4a. [x] OpenIV-style browsing: folder/archive tree → contents list → double-click to preview,
   Backspace back, clickable path bar, search within the open folder, Properties (`Alt+Enter`)
   - [x] Previews open in their own windows (toolbar for per-file tools, drop-to-replace)
   - [x] Game's .exe icon on the tree's top node; optional files in the tree
   - [x] Open archives / any folder alongside the game (extra top-level entries, remembered per game)
   - [x] Per-file Export / Import buttons in the preview window
   - [ ] Per-file tools in the preview window: image channels, mip selector
4b. [x] Replace assets in place (dialog, toolbar, drag & drop), Restore Original
   - [x] Edit Mode toggle (OpenIV-style, confirmation, status badge) gating all writes
   - [x] Explicit backups: red "not backed up" banner + Back Up Now; no automatic backups
   - [x] Backups kept in `<game>/backup/` (same layout; old `.bak` files still recognised);
     Restore Original on a file inside an archive puts back just that file
   - [ ] Test replaced Blur files in-game (normal archives, signed gamedata archives)
   - [ ] Batch replace: drop a folder whose layout mirrors the archive
5. [x] Path bar above the list (clickable breadcrumb) and View ▸ Go to Path… (`Ctrl+L`): type or paste
   a path (backslashes, a full disk path into the game folder), suggestions as you type
6. [ ] Hex view: selection, copy as hex/C array, go-to-offset, find bytes
7. [ ] Image view: channel toggles (R/G/B/A), mip selector, export as PNG
8. [x] Export converted formats (sound banks → .wav folders, models → .obj, textures →
   .png/.dds/.jpg/.bmp) and Import back (.wav folder → bank, picture → texture with DXT1/3/5
   or uncompressed, the original format by default; DXT encoder in core)
9. [ ] Parallel extraction (several files at once; archives already support concurrent reads)
10. [ ] Settings dialog (default extract folder, preview size limits)
11. [ ] Release workflow: tag → CI builds `dist/release` → zip attached to a GitHub Release

## Phase 4 — Deeper Blur support

1. [x] Levels: `.levelrender` + `.levelstream` (`ARCH`) decoded; every track opens in the 3D
   viewer (static world + placed props, all 19 levels)
   - [x] Level textures (texture blocks, mip chains rebuilt from levels 0–9, the render
     file's own textures), alpha from the game's render states, vertex alpha for decals
   - [x] Lightmaps (`g_tLightmap`; coordinates in TEXCOORD0.zw) with the game's own formula
     (its MaterialLOD pass); the frame's tone mapping / colour grading isn't reproduced
   - [ ] The ocean, detail layers, normal/specular maps
   - [ ] `Level.level` (adverts, navigation data, build log) as text
2. [x] `.model` geometry: vertex/index buffers, all LODs, shadow hulls (see docs/formats/blur.md); next: part names, materials, UVs
3. [x] 3D preview: orbit camera, parts tree, solid/wireframe/grid/shadow, LOD picker (OpenGL)
   - [x] Real normals and UVs; material look per effect (paint colour, glass, lights, chrome); embedded textures listed and exported
   - [x] Material values (which texture each material uses) → textured rendering, cars
     included (texture table recovered from Blur.exe)
   - [x] Skinned characters: decode `.skeleton` so heads/hands sit on their bones
   - [x] Animation player for every model: `.cycle` decoding (all 805 in `root.pak`), skinned
     and rigid parts (vehicles, props), play/reverse/step/speed/timeline, picker tree showing
     only animations that fit the model's skeleton
   - [x] Studio lighting like the game's: key and fill lights, sky/ground ambient,
     per-material highlights, sky reflections at grazing angles (paint, chrome, glass)
   - [ ] Translate the game's effects (`shaders-speed.pak`) for an exact match
   - [ ] Add new textures to a model (insert into the scene stream, texture table and
     material values; the per-texture ID is the exporter's, so a new one needs an unused value)
   - [x] Collision: a model's `Coll` shapes drawn over it ("Collision"); `.levelphysics`
     opens in the 3D viewer, coloured by surface material, one route layout at a time
4. [x] `.baf` `BANK` audio → WAV export and playback (Qt Multimedia); import WAV folders and
   replace single sounds (ADPCM encoder; every game bank rebuilds byte for byte)
4b. [x] Bink `.bik` video playback (Qt Multimedia FFmpeg backend), audio track picker
4c. [x] Text viewer/editor: colour coding (XML, INI, CSV, JSON, Lua, shaders, QuickBMS), encodings round-trip, saving through Edit Mode
4d. [x] Audio entities (`audio/dat/*.dat`, JBOA) decompiled to text (preview, Export, CLI);
   [x] edited numbers compile back; next: name the fields per component type
5. [x] Data tables (`.bin`, `.xt` "Flask" packs incl. car handling; `.loc` texts) → decompiled text
   - [x] Editable: Edit on any decompiled view; Save compiles the values back (core `TextEdit`)
   - [x] Longer/shorter lists in data tables (new items copy the last one, with its linked objects)
   - [ ] New or deleted records; first item of an empty list of links
   - [ ] Test edited gamedata in-game (gamedata.pak has a .sig file)
   - [ ] `.aid` AI route data
6. [x] UI files (`cache/root.pak/ui/`): `.ipf` vector layouts (75: icon sheets, HUD, minimaps
   — drawn as pictures, items side by side with their names), `ui_font_eur.dat` (font atlas
   table, as text)
   - [x] `.bpr` (`2RZB`) UI texture packs (4, ~300 textures): contact-sheet preview,
     Textures… window to browse / export / replace
   - [x] `.pf` fonts (5): vector outlines; preview with your own sample text ("The quick
     brown fox…") at four sizes and every character in a grid
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
