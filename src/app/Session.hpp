#pragma once

#include "viewer/core/GameRegistry.hpp"
#include "viewer/core/Vfs.hpp"

#include <QString>

#include <filesystem>
#include <memory>
#include <vector>

namespace viewer::app {

/// Everything loaded for one browsing session: the chosen game (if any), its
/// registered content handlers and the mounted virtual file system.
///
/// Besides the main mount (game install, archive or folder) a session can hold
/// extra mounts the user added alongside it — more archives or any folder
/// (a mod workspace, say). Each is its own top-level entry in the tree and
/// uses the same content handlers as the main mount.
///
/// Sessions are plain C++ objects, built on a worker thread and then handed
/// to the UI thread, so opening a big install never freezes the window.
struct Session {
    /// An archive or folder opened alongside the main mount.
    struct Extra {
        std::filesystem::path source;
        std::unique_ptr<Vfs> vfs;
        const VfsNode* top = nullptr; ///< Shown in the tree: the folder, or the archive node.
        MountReport report;
    };

    const Game* game = nullptr;   ///< Null when browsing a standalone archive or folder.
    std::filesystem::path source; ///< Install folder, archive file or folder.
    ContentRegistry content;
    Vfs vfs;
    MountReport report;
    std::vector<std::shared_ptr<Extra>> extras;

    [[nodiscard]] QString title() const;
    /// Sources of the extra mounts, in order (to remount them after a reload).
    [[nodiscard]] std::vector<std::filesystem::path> extraSources() const;
};

/// Mounts a game's install folder with that game's handlers.
std::unique_ptr<Session> openGameSession(const Game& game, const std::filesystem::path& directory);

/// Mounts a single archive using every compiled-in game's handlers.
Result<std::unique_ptr<Session>> openArchiveSession(const GameRegistry& games, const std::filesystem::path& file);

/// Mounts a folder that isn't a known game install, using every compiled-in
/// game's handlers.
std::unique_ptr<Session> openFolderSession(const GameRegistry& games, const std::filesystem::path& directory);

/// Mounts an archive or folder to be added to a session that uses `content`.
/// Thread-safe as long as `content` isn't modified meanwhile.
Result<std::unique_ptr<Session::Extra>> mountExtra(const ContentRegistry& content, const std::filesystem::path& source);

} // namespace viewer::app
