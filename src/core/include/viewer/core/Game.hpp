#pragma once

#include "viewer/core/ContentRegistry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace viewer {

struct GameInfo {
    std::string id;          ///< Stable key used in settings: "blur".
    std::string title;       ///< "Blur".
    std::string developer;   ///< "Bizarre Creations".
    int year = 0;
    std::string description; ///< One line shown in the game picker.
    std::string executable;  ///< Main executable, relative to the install ("Blur.exe"); its icon marks the game.
};

/// A game module: knows how to recognise an install and which formats,
/// decoders and file types apply to it.
class Game {
public:
    virtual ~Game() = default;

    [[nodiscard]] virtual const GameInfo& info() const noexcept = 0;

    /// True if `directory` looks like an install of this game.
    [[nodiscard]] virtual bool detectInstall(const std::filesystem::path& directory) const = 0;

    /// Likely install locations to pre-fill the picker (registry, defaults...).
    [[nodiscard]] virtual std::vector<std::filesystem::path> suggestInstallPaths() const { return {}; }

    /// Adds this game's archive formats, decoders and file types.
    virtual void registerContent(ContentRegistry& registry) const = 0;

    /// Directories (relative to the install) to skip while scanning, e.g. caches.
    [[nodiscard]] virtual std::vector<std::string> ignoredDirectories() const { return {}; }
};

} // namespace viewer
