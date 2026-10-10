// 26viewer-cli — scriptable front-end over the same core + game modules the
// GUI uses. Handy for bulk extraction, CI checks and format research.

#include "games/GameModules.hpp"
#include "viewer/core/GameRegistry.hpp"
#include "viewer/core/RandomAccessFile.hpp"
#include "viewer/core/Vfs.hpp"

#include <chrono>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace viewer;

namespace {

std::vector<std::string> utf8Arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    std::vector<std::string> args;
#ifdef _WIN32
    // argv is in the ANSI code page on Windows; fetch the real UTF-16 command line.
    int count = 0;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 0; i < count; ++i) {
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(std::max(0, len - 1)), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), len, nullptr, nullptr);
            args.push_back(std::move(s));
        }
        ::LocalFree(wide);
        return args;
    }
#endif
    for (int i = 0; i < argc; ++i)
        args.emplace_back(argv[i]);
    return args;
}

fs::path toPath(const std::string& utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string toUtf8(const fs::path& p)
{
    auto s = p.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

int usage()
{
    std::println(stderr, R"(26viewer-cli — game archive tool

Usage:
  26viewer-cli games                              List compiled-in game modules
  26viewer-cli info    <archive>                  Show archive properties
  26viewer-cli list    <archive|game dir> [filter] List files (optional substring filter)
  26viewer-cli verify  <archive|game dir>         Decompress every file and report errors
  26viewer-cli extract <archive|game dir> <out> [filter]
                                                  Extract files, keeping folder structure)");
    return 2;
}

struct Session {
    GameRegistry games;
    ContentRegistry content;
    Vfs vfs;
    const Game* game = nullptr;
};

/// Loads every game's handlers (the CLI doesn't know which game a lone
/// archive belongs to) and mounts the target.
Result<const VfsNode*> mount(Session& s, const fs::path& target)
{
    registerBuiltinGames(s.games);
    registerBuiltinContent(s.content);

    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        s.game = s.games.detect(target);
        if (s.game) {
            s.game->registerContent(s.content);
        } else {
            for (const auto& g : s.games.games())
                g->registerContent(s.content);
        }
        MountOptions options;
        if (s.game)
            options.ignoredDirectories = s.game->ignoredDirectories();
        auto report = s.vfs.mountDirectory(target, s.content, options);
        for (const auto& w : report.warnings)
            std::println(stderr, "warning: {}", w.message);
        return &s.vfs.root();
    }
    for (const auto& g : s.games.games())
        g->registerContent(s.content);
    return s.vfs.mountArchive(target, s.content);
}

bool matches(const VfsNode& node, const std::string& filter)
{
    return filter.empty() || toLower(node.virtualPath()).find(toLower(filter)) != std::string::npos;
}

int cmdGames()
{
    GameRegistry games;
    registerBuiltinGames(games);
    for (const auto& g : games.games()) {
        const auto& i = g->info();
        std::println("{:<10} {} ({}, {}) — {}", i.id, i.title, i.developer, i.year, i.description);
    }
    return 0;
}

int cmdInfo(const fs::path& target)
{
    Session s;
    auto node = mount(s, target);
    if (!node) {
        std::println(stderr, "error: {}", node.error().message);
        return 1;
    }
    const Archive* archive = (*node)->archive();
    if (!archive) {
        std::println(stderr, "error: not an archive");
        return 1;
    }
    for (const auto& p : archive->describe())
        std::println("{:<20} {}", p.name, p.value);
    std::println("{:<20} {}", "Unpacked size", formatSize((*node)->size()));
    return 0;
}

int cmdList(const fs::path& target, const std::string& filter)
{
    Session s;
    auto node = mount(s, target);
    if (!node) {
        std::println(stderr, "error: {}", node.error().message);
        return 1;
    }
    std::size_t shown = 0;
    for (const VfsNode* f : Vfs::collectFiles(**node)) {
        if (!matches(*f, filter))
            continue;
        std::println("{:>12} {:>12}  {}", f->size(), f->packedSize(), f->virtualPath());
        ++shown;
    }
    std::println(stderr, "{} file(s)", shown);
    return 0;
}

int cmdVerify(const fs::path& target)
{
    Session s;
    auto node = mount(s, target);
    if (!node) {
        std::println(stderr, "error: {}", node.error().message);
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t ok = 0, failed = 0;
    std::uint64_t bytes = 0;
    for (const VfsNode* f : Vfs::collectFiles(**node)) {
        if (!f->archive())
            continue; // only packed files need verifying
        auto data = s.vfs.read(*f);
        if (data && data->size() == f->size()) {
            ++ok;
            bytes += data->size();
        } else {
            ++failed;
            std::println(stderr, "FAIL {}: {}", f->virtualPath(),
                         data ? std::format("size {} != {}", data->size(), f->size()) : data.error().message);
        }
    }
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::println("{} ok, {} failed, {} decompressed in {:.2f}s", ok, failed, formatSize(bytes), secs);
    return failed == 0 ? 0 : 1;
}

int cmdExtract(const fs::path& target, const fs::path& outDir, const std::string& filter)
{
    Session s;
    auto node = mount(s, target);
    if (!node) {
        std::println(stderr, "error: {}", node.error().message);
        return 1;
    }
    std::size_t done = 0, failed = 0;
    for (const VfsNode* f : Vfs::collectFiles(**node)) {
        if (!matches(*f, filter))
            continue;
        const fs::path dest = outDir / sanitizeRelativePath(f->pathRelativeTo(*node));
        auto data = s.vfs.read(*f);
        auto written = data ? writeFile(dest, *data) : Result<void>(std::unexpected(data.error()));
        if (written) {
            ++done;
        } else {
            ++failed;
            std::println(stderr, "FAIL {}: {}", f->virtualPath(), written.error().message);
        }
    }
    std::println("extracted {} file(s) to {}{}", done, toUtf8(outDir),
                 failed ? std::format(", {} failed", failed) : "");
    return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    const auto args = utf8Arguments(argc, argv);
    if (args.size() < 2)
        return usage();
    const std::string& cmd = args[1];
    auto arg = [&](std::size_t i) { return i < args.size() ? args[i] : std::string{}; };

    if (cmd == "games")
        return cmdGames();
    if (args.size() < 3)
        return usage();
    if (cmd == "info")
        return cmdInfo(toPath(args[2]));
    if (cmd == "list")
        return cmdList(toPath(args[2]), arg(3));
    if (cmd == "verify")
        return cmdVerify(toPath(args[2]));
    if (cmd == "extract" && args.size() >= 4)
        return cmdExtract(toPath(args[2]), toPath(args[3]), arg(4));
    return usage();
}
