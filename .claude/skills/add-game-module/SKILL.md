---
name: add-game-module
description: Step-by-step recipe for adding support for a new game to 26Viewer (module folder, Game class, archive formats, decoders, CMake registration, tests, docs). Use when the user wants another game supported or a new archive/file format wired in.
---

# Adding a game module

Read `docs/ARCHITECTURE.md` first if you haven't this session. Formats should
already be understood (see the `reverse-engineer-format` skill).

## Decide what is global vs game-specific

- Reusable by any game (codecs, image formats, generic containers like ZIP) → `src/core/`.
- Shared by an engine/studio family → `src/games/shared/<family>/` (create it the
  first time two games need the same code; e.g. Bizarre's chunk format in
  `src/games/blur/ChunkFile.*` is the first candidate).
- Only this game → `src/games/<game>/`.

## Files to create (`<game>` = lowercase identifier, e.g. `pgr4`)

`src/games/<game>/CMakeLists.txt`
```cmake
add_library(viewer_game_<game> STATIC
    <Game>Game.cpp
    <Format>Archive.cpp
    <Format>Archive.hpp)
target_include_directories(viewer_game_<game> PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(viewer_game_<game> PUBLIC viewer::core)
```

`src/games/<game>/<Game>Game.cpp` — model it on `src/games/blur/BlurGame.cpp`:
- `info()` → id (must equal the folder name), title, developer, year, description.
- `detectInstall(dir)` → check 1–2 files that only this game has (exe + a key archive).
- `suggestInstallPaths()` → common install folders (optional).
- `registerContent(registry)` → `addArchiveFormat`, `addImageDecoder`,
  `addStructureParser`, and `fileTypes().add({ext, description, FileCategory::…})`
  for every extension the game uses.
- `void registerModule(GameRegistry& r) { r.add(std::make_unique<…Game>()); }`
  inside `namespace viewer::games::<game>`.

Archive classes — model them on `src/games/blur/PakArchive.*`:
- `ArchiveFormat::probe()` checks magic bytes only (cheap, ≤ 4 KiB provided).
- `Archive::open()` reads the directory once; `read(i)` must be thread-safe
  (use `RandomAccessFile::read`, no shared mutable state).
- Normalise names with `normalizeArchivePath`, timestamps with `fromFileTime` (if FILETIME).
- Skip directory records; the VFS builds folders from paths.
- Fill `describe()`/`describeEntry()` with format details for the Properties tab.
- Optional writing: override `canReplace()` + `replace(index, data)` (see
  `PakArchive::replace`): encode like the original entry, append at the end,
  patch only that entry's record *last*. Read-only handling is done by
  `Vfs::replace`; backups are always the user's explicit choice (Edit Mode banner /
  `ensureBackup`) — never back up or write outside Edit Mode automatically. Test round-trips on synthetic archives and
  on **copies** of real archives in the scratchpad — never on the user's install.

## Register and build

```bash
cmake --preset debug -DVIEWER_GAMES="blur;<game>"
cmake --build --preset debug
```
To make it default, edit `VIEWER_GAMES` in the root `CMakeLists.txt`. No other
shared code changes are needed — `cmake/Games.cmake` generates the registration.

## Test and verify

1. `tests/test_<game>_*.cpp` building the format in memory (pattern: `tests/test_blur_pak.cpp`);
   add the files to `tests/CMakeLists.txt` and link `viewer_game_<game>`.
2. `ctest --preset debug`.
3. `26viewer-cli verify <install dir>` → 0 failures.
4. GUI screenshot via the `build-and-run` skill, light and dark.

## Document

- `docs/formats/<game>.md` (format spec with status markers).
- README "Supported games" table row.
- Tick/add items in `docs/ROADMAP.md`.
