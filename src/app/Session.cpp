#include "Session.hpp"

#include "QtConvert.hpp"

namespace viewer::app {

QString Session::title() const
{
    if (game)
        return toQString(game->info().title);
    return toQString(source.filename());
}

std::unique_ptr<Session> openGameSession(const Game& game, const std::filesystem::path& directory)
{
    auto session = std::make_unique<Session>();
    session->game = &game;
    session->source = directory;
    registerBuiltinContent(session->content);
    game.registerContent(session->content);

    MountOptions options;
    options.ignoredDirectories = game.ignoredDirectories();
    session->report = session->vfs.mountDirectory(directory, session->content, options);
    return session;
}

Result<std::unique_ptr<Session>> openArchiveSession(const GameRegistry& games, const std::filesystem::path& file)
{
    auto session = std::make_unique<Session>();
    session->source = file;
    registerBuiltinContent(session->content);
    for (const auto& g : games.games())
        g->registerContent(session->content);

    auto node = session->vfs.mountArchive(file, session->content);
    if (!node)
        return std::unexpected(node.error());
    session->report.archives = 1;
    session->report.files = (*node)->fileCount();
    return session;
}

} // namespace viewer::app
