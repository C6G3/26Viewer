#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace viewer::mcp {

struct Options {
    /// A game's install folder (or one archive) to work on.
    std::filesystem::path target;
    /// The tools that change game files (edit_text, replace_file, backup) are
    /// only offered with this on. Even then a file is only written once its
    /// archive has a backup (made with the backup tool).
    bool allowEdits = false;
    /// The version reported to clients.
    std::string version = "0";
};

/// A Model Context Protocol server (https://modelcontextprotocol.io) over the
/// same core and game modules as the app: lets an AI assistant (Claude, VS Code
/// Copilot's agent mode, Cursor…) browse the game, read decompiled files,
/// search them, look up names and hashes, inspect models, extract files and —
/// when allowed — edit them. Speaks JSON-RPC 2.0, one message per line.
class Server {
public:
    explicit Server(Options options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Handles one message; returns the reply, or nothing (notifications).
    [[nodiscard]] std::optional<std::string> handle(std::string_view message);

    /// Reads messages from `in` (one per line) until it ends, writing replies to `out`.
    void run(std::istream& in, std::ostream& out);

private:
    struct State;
    std::unique_ptr<State> m;
};

} // namespace viewer::mcp
