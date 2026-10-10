#pragma once

#include "viewer/core/Game.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace viewer {

/// The list of games compiled into this build.
class GameRegistry {
public:
    void add(std::unique_ptr<Game> game);

    [[nodiscard]] std::span<const std::unique_ptr<Game>> games() const noexcept { return m_games; }
    [[nodiscard]] const Game* find(std::string_view id) const noexcept;
    /// First game whose detectInstall() accepts `directory`, if any.
    [[nodiscard]] const Game* detect(const std::filesystem::path& directory) const;

private:
    std::vector<std::unique_ptr<Game>> m_games;
};

} // namespace viewer
