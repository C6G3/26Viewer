#include "Session.hpp"

#include "QtConvert.hpp"

namespace viewer::app {

QString Session::title() const
{
    if (game)
        return toQString(game->info().title);
    return toQString(source.filename());
}

std::vector<std::filesystem::path> Session::extraSources() const
{
    std::vector<std::filesystem::path> sources;
    for (const auto& extra : extras)
        sources.push_back(extra->source);
    return sources;
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

std::unique_ptr<Session> openFolderSession(const GameRegistry& games, const std::filesystem::path& directory)
{
    auto session = std::make_unique<Session>();
    session->source = directory;
    registerBuiltinContent(session->content);
    for (const auto& g : games.games())
        g->registerContent(session->content);
    session->report = session->vfs.mountDirectory(directory, session->content);
    return session;
}

Result<std::unique_ptr<Session::Extra>> mountExtra(const ContentRegistry& content, const std::filesystem::path& source)
{
    auto extra = std::make_unique<Session::Extra>();
    extra->source = source;
    extra->vfs = std::make_unique<Vfs>();
    std::error_code ec;
    if (std::filesystem::is_directory(source, ec)) {
        extra->report = extra->vfs->mountDirectory(source, content);
        extra->top = &extra->vfs->root();
        return extra;
    }
    auto node = extra->vfs->mountArchive(source, content);
    if (!node)
        return std::unexpected(node.error());
    extra->report.archives = 1;
    extra->report.files = (*node)->fileCount();
    extra->top = *node; // show the archive itself, not an unnamed folder around it
    return extra;
}

} // namespace viewer::app
