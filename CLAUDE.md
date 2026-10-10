# 26Viewer — notes for Claude

Modular game archive viewer: C++23 core (no Qt) + game modules + Qt 6 Widgets app + CLI.
First game: Blur (install at `F:\Games\BLUR`).

- Plan and status: `docs/ROADMAP.md` (tick items when done). Architecture: `docs/ARCHITECTURE.md`.
- Format knowledge: `docs/formats/<game>.md`; Python reference readers in `tools/research/`.
- Skills in `.claude/skills/`: `build-and-run`, `reverse-engineer-format`, `add-game-module`, `add-previewer`.

## Conventions

- Layers: `src/core` (never includes Qt) ← `src/games/<name>` ← `src/app`, `src/cli`.
- Errors: return `Result<T>` (`std::expected`) via `fail(ErrorCode::…, "fmt", …)`; no exceptions across modules.
- Binary parsing: `BinaryReader` / `loadScalar` with explicit bounds checks.
- Strings are UTF-8 `std::string`; paths are `std::filesystem::path`; convert to Qt with `app/QtConvert.hpp`.
- `Archive::read` must be thread-safe; workers capture `std::shared_ptr<Session>`.
- UI colours come from the palette (dark mode must keep working); verify UI changes with
  `--screenshot` in both `--theme light` and `--theme dark`.
- Sources are listed explicitly in each `CMakeLists.txt`.
- Keep the build warning-free and `ctest` green. Never commit game data, `build/` or `dist/`.
- The user is new to C++/GUI work: explain changes in plain language.
