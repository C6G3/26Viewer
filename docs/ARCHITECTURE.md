# Architecture

26Viewer is built in layers. Each layer only depends on the ones below it, which
is what lets new games reuse everything that isn't specific to them.

```
┌──────────────────────────────┐  ┌──────────────────┐
│ src/app  (Qt 6 Widgets GUI)  │  │ src/cli          │   front-ends
└──────────────┬───────────────┘  └────────┬─────────┘
               │      registerBuiltinGames()│
┌──────────────▼────────────────────────────▼─────────┐
│ src/games/<name>   one static library per game      │   game modules
│   (Blur: PakArchive, ChunkFile, TextureDecoder)     │
└──────────────┬──────────────────────────────────────┘
               │ implements interfaces from
┌──────────────▼──────────────────────────────────────┐
│ src/core   game-agnostic, no Qt                     │   foundation
│   Archive / ArchiveFormat     Game / GameRegistry   │
│   ContentRegistry (formats, decoders, file types)   │
│   Vfs (tree of folders, archives, files)            │
│   Image decoders (DDS, BC1–5, D3D formats)          │
│   BinaryReader, RandomAccessFile, Compression       │
└─────────────────────────────────────────────────────┘
```

## Core concepts

| Type | Role |
|---|---|
| `Result<T>` | `std::expected<T, Error>`. Every fallible call returns one; exceptions never cross module boundaries. |
| `BinaryReader` | Bounds-checked reads from a byte span. Parsers never index raw memory unchecked. |
| `RandomAccessFile` | Positional reads (`ReadFile`+`OVERLAPPED` / `pread`), safe from many threads with no lock. |
| `Archive` | An opened archive: `entries()`, `read(i)`, `describeEntry(i)`. Must be thread-safe for reads. |
| `ArchiveFormat` | Factory: `extensions()`, `probe(header)`, `open(path)`. |
| `ImageDecoder` / `StructureParser` | Turn file bytes into an `Image` (RGBA8) or a `StructureNode` tree. |
| `FileTypeRegistry` | Extension → description + `FileCategory` (drives icons and previewers). |
| `ContentRegistry` | Holds all of the above for a session. Built-ins first, then the game's; lookups go newest-first so a game can override a global handler. |
| `Game` | A game module: `info()`, `detectInstall(dir)`, `suggestInstallPaths()`, `registerContent(registry)`. |
| `GameRegistry` | Games compiled into this build. |
| `Vfs` / `VfsNode` | The browsable tree. Mirrors the install folder; recognised archives become expandable nodes. |

### Global vs game-specific

Anything that more than one game could use goes in `src/core` (or a future
`src/games/shared/<family>` for engine families). Examples:

- **Global**: DDS/BC decoding, zlib/deflate, text detection, VFS, extraction, every UI widget.
- **Blur-specific**: the 2KAP container, the XOR name key, `.texture` wrapper.
- **Engine family** (candidate for promotion): the Bizarre Creations chunk format
  (`ChunkFile.cpp`) is likely shared with other Bizarre games — move it to
  `src/games/shared/bizarre/` when a second game needs it.

## Build-time game selection

`VIEWER_GAMES` (CMake cache list, default `blur`) chooses which modules compile
in. `cmake/Games.cmake` validates each name, adds `src/games/<name>`, and
generates `GameModules.cpp`, which calls every module's `registerModule()`.
No shared code changes when a game is added or removed.

## Runtime flow

1. `main` registers built-in games, creates `ThemeManager` and `MainWindow`.
2. `GamePickerDialog` → the user picks a game and folder (remembered in
   `%APPDATA%/26Viewer/26Viewer.ini`).
3. `openGameSession()` runs on a worker thread (`QtConcurrent::run`): builds a
   `ContentRegistry` (built-ins + game), then `Vfs::mountDirectory()`.
4. The finished `Session` (held by `std::shared_ptr`) is attached to `VfsModel`
   → `VfsFilterModel` (sorting, recursive filtering) → `QTreeView`.
5. Selecting a file: `PreviewPane` starts a background load (read → decode image
   → parse structure → detect text). A generation counter drops stale results.
6. Extraction: `startExtraction()` runs on a worker with `QPromise` progress and
   cancellation; the session's `shared_ptr` keeps archives alive meanwhile.

## Threading rules

- Archives are opened once and read concurrently, so `Archive::read` must not mutate shared state.
- Worker lambdas capture the `shared_ptr<Session>`, never raw pointers to it.
- Only the UI thread touches widgets/models; results come back via `QFutureWatcher`.

## Theming

- Qt 6.10 uses the `windows11` style on Windows 11 (Fluent look).
- *System* mode calls `QStyleHints::unsetColorScheme()`, so Qt follows the OS
  setting live, including the dark title bar.
- *Light*/*Dark* call `QStyleHints::setColorScheme()` (Qt 6.8+).
- Custom-painted widgets (`HexView`, `ImageCanvas`) read colours from the
  palette at paint time and rebuild cached brushes on `QEvent::PaletteChange`.
- File-type icons are SVGs with mid-saturation accents that work on both backgrounds.

## Adding a game

1. **Research** the formats: hex-dump headers, prototype a reader in
   `tools/research/<game>_*.py`, and verify it over *every* archive the game ships.
   Document the result in `docs/formats/<game>.md`.
2. **Create the module** `src/games/<game>/`:
   - `CMakeLists.txt`: `add_library(viewer_game_<game> STATIC …)` linking `viewer::core`.
   - `<Game>Game.cpp`: a `Game` subclass and
     `void viewer::games::<game>::registerModule(GameRegistry& r) { r.add(std::make_unique<…>()); }`.
   - One `ArchiveFormat` + `Archive` per container format.
   - Optional `ImageDecoder` / `StructureParser` for game-specific files.
   - `registerContent()` registers all of the above and the game's file types.
3. **Enable it**: `-DVIEWER_GAMES="blur;<game>"` (or change the default in `CMakeLists.txt`).
4. **Test it**: synthetic-archive tests in `tests/` (see `test_blur_pak.cpp`),
   plus `26viewer-cli verify <install>` against the real game.
5. **Document**: add a row to the README's supported games table.
