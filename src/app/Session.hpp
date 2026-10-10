#pragma once

#include "viewer/core/GameRegistry.hpp"
#include "viewer/core/Vfs.hpp"

#include <QString>

#include <filesystem>
#include <memory>

namespace viewer::app {

/// Everything loaded for one browsing session: the chosen game (if any), its
/// registered content handlers and the mounted virtual file system.
///
/// Sessions are plain C++ objects, built on a worker thread and then handed
/// to the UI thread, so opening a big install never freezes the window.
struct Session {
    const Game* game = nullptr;   ///< Null when browsing a standalone archive.
    std::filesystem::path source; ///< Install folder or archive file.
    ContentRegistry content;
    Vfs vfs;
    MountReport report;

    [[nodiscard]] QString title() const;
};

/// Mounts a game's install folder with that game's handlers.
std::unique_ptr<Session> openGameSession(const Game& game, const std::filesystem::path& directory);

/// Mounts a single archive using every compiled-in game's handlers.
Result<std::unique_ptr<Session>> openArchiveSession(const GameRegistry& games, const std::filesystem::path& file);

} // namespace viewer::app
