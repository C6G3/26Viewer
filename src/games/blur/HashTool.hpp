#pragma once

#include "viewer/core/ContentRegistry.hpp"

namespace viewer::games::blur {

/// Blur's names and numbers:
///   * texts (LocId): the ids the text tables in gamedata/xt/loc give each
///     text's name. The game never computes them (it looks names up in the
///     tables), so a name the tables don't have gets the id 26Viewer would give
///     a new text called that (see newTextId).
///   * archive paths: the .pak name hash, ~CRC-32 of the lower-case "\path".
/// A number is looked up among the names the open game's files hold.
class BlurHashTool final : public HashTool {
public:
    std::string_view name() const noexcept override { return "Blur names and hashes"; }
    std::string_view description() const noexcept override;
    std::vector<HashMatch> lookup(std::string_view query, const HashToolContext& context) const override;
};

} // namespace viewer::games::blur
