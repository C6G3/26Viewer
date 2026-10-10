#include "viewer/core/GameRegistry.hpp"

namespace viewer {

void GameRegistry::add(std::unique_ptr<Game> game)
{
    if (game)
        m_games.push_back(std::move(game));
}

const Game* GameRegistry::find(std::string_view id) const noexcept
{
    for (const auto& g : m_games)
        if (g->info().id == id)
            return g.get();
    return nullptr;
}

const Game* GameRegistry::detect(const std::filesystem::path& directory) const
{
    for (const auto& g : m_games)
        if (g->detectInstall(directory))
            return g.get();
    return nullptr;
}

} // namespace viewer
