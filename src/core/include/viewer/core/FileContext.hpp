#pragma once

#include "viewer/core/Bytes.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// Other files a decoder may consult: names for the ids a table holds, or
/// the second half of a format split over two files (a Blur level's
/// .levelrender and .levelstream). Every member may be empty (a file opened
/// on its own).
struct DecompileContext {
    struct File {
        std::string name;
        ByteBuffer data;
    };
    /// The files in a folder, given relative to the archive (or the game
    /// folder) holding the decoded file: "gamedata/xt/loc".
    std::function<std::vector<File>(std::string_view folder)> readFolder;
    /// The whole of a file in the same folder as the decoded one (the name
    /// is matched without regard to case), or nullopt if there is none.
    std::function<std::optional<ByteBuffer>(std::string_view name)> readSibling;
};

/// What a game's hash tool (HashTool) may consult: the whole open game.
struct HashToolContext {
    /// The files of a folder in every open archive and the game folder:
    /// "gamedata/xt/loc".
    std::function<std::vector<DecompileContext::File>(std::string_view folder)> readFolder;
    /// The path of every file and folder inside the open archives, relative to
    /// its archive, with / separators: "models/vehicles/a.model".
    std::function<std::vector<std::string>()> archivePaths;
};

} // namespace viewer
