// The MCP server: JSON-RPC 2.0 messages, one per line (the stdio transport of
// https://modelcontextprotocol.io). Methods: initialize, ping, tools/list,
// tools/call; notifications get no reply. Every tool answers with text meant
// for an AI assistant to read; problems come back as tool errors (isError)
// so the assistant can correct itself.

#include "McpServer.hpp"

#include "games/GameModules.hpp"
#include "viewer/core/GameRegistry.hpp"
#include "viewer/core/Json.hpp"
#include "viewer/core/Mesh.hpp"
#include "viewer/core/RandomAccessFile.hpp"
#include "viewer/core/Text.hpp"
#include "viewer/core/Vfs.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <istream>
#include <map>
#include <ostream>
#include <ranges>
#include <set>

namespace viewer::mcp {

namespace fs = std::filesystem;
using json::Array;
using json::Object;
using json::Value;

namespace {

constexpr std::size_t kMaxReply = 60'000; ///< Characters of tool text sent back at most.

/// Protocol versions this server speaks, newest first.
constexpr std::string_view kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

std::string utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

fs::path pathOf(std::string_view s)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string clipped(std::string text)
{
    if (text.size() <= kMaxReply)
        return text;
    std::size_t cut = text.rfind('\n', kMaxReply);
    if (cut == std::string::npos || cut < kMaxReply / 2)
        cut = kMaxReply;
    text.resize(cut);
    return text + "\n… (cut short: ask for less, e.g. a smaller max_lines or a narrower search)";
}

/// A JSON Schema object with these properties (name, type, description) and required names.
Value schema(std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view>> properties,
             std::initializer_list<std::string_view> required = {})
{
    Object props;
    for (const auto& [name, type, description] : properties) {
        Value p;
        if (type == "string[]") {
            p.set("type", "array");
            p.set("items", Object{{"type", "string"}});
        } else {
            p.set("type", type);
        }
        p.set("description", description);
        props.emplace_back(std::string(name), std::move(p));
    }
    Value s;
    s.set("type", "object");
    s.set("properties", std::move(props));
    Array req;
    for (std::string_view r : required)
        req.emplace_back(r);
    s.set("required", std::move(req));
    return s;
}

} // namespace

struct Server::State {
    using Handler = std::function<Result<std::string>(const Value& args)>;
    struct Tool {
        std::string name, title, description;
        Value inputSchema;
        bool edits = false;
        Handler run;
    };

    Options options;
    GameRegistry games;
    ContentRegistry content;
    std::unique_ptr<Vfs> vfs;
    const Game* game = nullptr;
    std::string mountError;
    std::map<std::string, std::string> texts; ///< virtual path -> its text (decompiled), cached
    std::set<std::string> binaries;           ///< virtual paths found not to be text
    /// (archive, folder) -> its files, as read for decompilers: game data looks up
    /// the text tables (several MB) for every file, far too slow for a search
    /// without this.
    std::map<std::pair<const VfsNode*, std::string>, std::vector<DecompileContext::File>> folderCache;
    std::vector<Tool> tools;

    explicit State(Options o) : options(std::move(o))
    {
        registerBuiltinGames(games);
        registerBuiltinContent(content);
        std::error_code ec;
        if (fs::is_directory(options.target, ec))
            game = games.detect(options.target);
        if (game)
            game->registerContent(content);
        else
            for (const auto& g : games.games())
                g->registerContent(content);
        mount();
        addTools();
    }

    void mount()
    {
        texts.clear();
        binaries.clear();
        folderCache.clear();
        vfs = std::make_unique<Vfs>();
        mountError.clear();
        std::error_code ec;
        if (fs::is_directory(options.target, ec)) {
            MountOptions mo;
            if (game)
                mo.ignoredDirectories = game->ignoredDirectories();
            (void)vfs->mountDirectory(options.target, content, mo);
            return;
        }
        if (!fs::exists(options.target, ec)) {
            mountError = std::format("{} doesn't exist", utf8(options.target));
            return;
        }
        for (fs::path dir = fs::absolute(options.target, ec).parent_path(); !dir.empty(); dir = dir.parent_path()) {
            if (games.detect(dir)) {
                vfs->setBackupRoot(dir);
                break;
            }
            if (dir == dir.parent_path())
                break;
        }
        if (auto r = vfs->mountArchive(options.target, content); !r)
            mountError = r.error().message;
    }

    // ---- finding files ------------------------------------------------------------------

    std::vector<const VfsNode*> allFiles() const { return Vfs::collectFiles(vfs->root()); }

    /// A file or folder by its path ("cache/root.pak/models/a.model"; any case,
    /// / or \); or the one file whose path ends with what was given.
    Result<const VfsNode*> find(std::string_view raw, bool folders = false) const
    {
        std::string path(raw);
        std::ranges::replace(path, '\\', '/');
        while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        const VfsNode* node = &vfs->root();
        if (path.empty())
            return node;
        for (const auto part : std::views::split(path, '/')) {
            const std::string name = toLower(std::string_view(part.begin(), part.end()));
            const VfsNode* next = nullptr;
            for (const auto& child : node->children())
                if (toLower(child->name()) == name)
                    next = child.get();
            node = next;
            if (!node)
                break;
        }
        if (node && (folders || node->isFile()))
            return node;
        // The end of a path: "dodge_viperacr_stock.model", "xt/amax.bin".
        const std::string tail = "/" + toLower(path);
        std::vector<const VfsNode*> hits;
        for (const VfsNode* f : allFiles())
            if (("/" + toLower(f->virtualPath())).ends_with(tail))
                hits.push_back(f);
        if (hits.size() == 1)
            return hits.front();
        if (hits.empty())
            return fail(ErrorCode::NotFound, "no file '{}' (use list_files to see the paths)", raw);
        std::string list;
        for (std::size_t i = 0; i < hits.size() && i < 12; ++i)
            list += "\n  " + hits[i]->virtualPath();
        return fail(ErrorCode::InvalidFormat, "'{}' could be {} files; give more of the path:{}", raw, hits.size(),
                    list);
    }

    std::string typeOf(const VfsNode& f) const
    {
        const FileTypeInfo* t = content.fileTypes().find(extensionOf(f.name()));
        return t ? t->description : std::string();
    }

    /// What decompilers get for `f`, with folder reads cached (until the next mount).
    DecompileContext contextFor(const VfsNode& f)
    {
        DecompileContext context = vfs->decompileContext(f);
        const VfsNode* archive = &f;
        while (archive->parent() && archive->kind() != VfsNode::Kind::Archive)
            archive = archive->parent();
        context.readFolder = [this, archive, read = std::move(context.readFolder)](std::string_view folder) {
            auto key = std::pair(archive, toLower(folder));
            auto it = folderCache.find(key);
            if (it == folderCache.end())
                it = folderCache.emplace(std::move(key), read(folder)).first;
            return it->second;
        };
        return context;
    }

    // ---- a file as text ----------------------------------------------------------------

    /// The file as text: decompiled (game data, texts, audio entities…) or a
    /// text file; nullopt for other binary files.
    Result<std::optional<std::string>> textOf(const VfsNode& f)
    {
        const std::string key = f.virtualPath();
        if (const auto it = texts.find(key); it != texts.end())
            return std::optional<std::string>(it->second);
        if (binaries.contains(key))
            return std::optional<std::string>();
        const std::string ext = extensionOf(f.name());
        // Big files (level streams are hundreds of MB) are only read whole when
        // their start says they are text or game data.
        constexpr std::uint64_t kBig = 16 * 1024 * 1024;
        if (f.size() > kBig) {
            auto head = vfs->read(f, 64 * 1024);
            if (!head)
                return std::unexpected(head.error());
            if (!content.findDecompiler(ext, *head) && !looksLikeText(f, *head)) {
                binaries.insert(key);
                return std::optional<std::string>();
            }
        }
        auto data = vfs->read(f);
        if (!data)
            return std::unexpected(data.error());
        std::optional<std::string> text;
        if (const TextDecompiler* d = content.findDecompiler(ext, *data)) {
            auto t = d->decompileWith(*data, contextFor(f));
            if (!t)
                return std::unexpected(t.error());
            text = std::move(*t);
        } else if (looksLikeText(f, *data)) {
            if (auto doc = decodeText(*data))
                text = std::move(doc->text);
        }
        if (text)
            texts.emplace(key, *text);
        else
            binaries.insert(key);
        return text;
    }

    bool looksLikeText(const VfsNode& f, ByteSpan data) const
    {
        const FileTypeInfo* t = content.fileTypes().find(extensionOf(f.name()));
        if (t && t->category == FileCategory::Text)
            return true;
        const std::size_t n = std::min<std::size_t>(data.size(), 4096);
        if (n == 0)
            return false;
        std::size_t odd = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto c = std::to_integer<unsigned char>(data[i]);
            if (c == 0)
                return false;
            odd += c < 0x09 || (c > 0x0D && c < 0x20);
        }
        return odd * 50 < n;
    }

    // ---- writing ---------------------------------------------------------------------------

    /// Writes `data` as the new contents of `f` (the archive must have a backup).
    Result<std::string> write(const VfsNode& f, ByteSpan data)
    {
        const fs::path target = Vfs::editTarget(f);
        if (!Vfs::canReplace(f))
            return fail(ErrorCode::Unsupported, "{} can't be written (its archive format can't be changed)",
                        f.virtualPath());
        if (findBackup(target, vfs->backupRoot()).empty())
            return fail(ErrorCode::InvalidFormat,
                        "{} has no backup yet; call the backup tool on this file first (the user keeps the "
                        "original that way), then try again",
                        utf8(target.filename()));
        const std::string where = f.virtualPath();
        auto report = vfs->replace(f, data);
        if (!report)
            return std::unexpected(report.error());
        std::string out = std::format("wrote {} ({} bytes, stored as {})", where, data.size(), report->storage);
        for (const auto& w : report->warnings)
            out += "\nnote: " + w;
        mount(); // the tree is stale after a write
        return out;
    }

    // ---- the tools -------------------------------------------------------------------------

    void add(std::string name, std::string title, std::string description, Value inputSchema, bool edits,
             Handler run)
    {
        tools.push_back({std::move(name), std::move(title), std::move(description), std::move(inputSchema), edits,
                         std::move(run)});
    }

    static std::size_t number(const Value& v, std::size_t fallback, std::size_t most)
    {
        const double d = v.asNumber(static_cast<double>(fallback));
        return d < 1 ? fallback : std::min(static_cast<std::size_t>(d), most);
    }

    void addTools();
};

void Server::State::addTools()
{
    add("overview", "Overview",
        "What is open: the game, its folder, its archives (with file counts), whether edits are allowed, and "
        "how paths look. Start here.",
        schema({}), false, [this](const Value&) -> Result<std::string> {
            if (!mountError.empty())
                return fail(ErrorCode::NotFound, "{}", mountError);
            std::string out = std::format("Game: {}\nOpened: {}\n", game ? game->info().title : "(no game recognised)",
                                          utf8(options.target));
            out += std::format("Edits: {}\n",
                               options.allowEdits ? "allowed (edit_text, replace_file, backup; an archive needs a "
                                                    "backup before it is written)"
                                                  : "off (start the server with --allow-edits to change files)");
            out += "\nTop level (path, files, size):\n";
            for (const auto& child : vfs->root().children())
                out += std::format("  {}{}  {} file(s), {}\n", child->name(), child->isFile() ? "" : "/",
                                   child->isFile() ? 1 : child->fileCount(), formatSize(child->size()));
            out += "\nPaths are as listed by list_files, e.g. \"cache/root.pak/models/vehicles/a.model\" (case "
                   "doesn't matter; the end of a path is enough when it is unique).\n"
                   "Game data tables (.bin, .xt), text tables (.loc) and audio entities read as text with read_file; "
                   "models are described by model_info.";
            return out;
        });

    add("list_files", "List files",
        "Lists files under a folder (default: everything), optionally only those whose path contains `filter` "
        "(case-insensitive). Shows each file's path, size and type.",
        schema({{"folder", "string", "Folder path to list under, e.g. \"cache/root.pak/models/vehicles\""},
                {"filter", "string", "Only paths containing this text, e.g. \"viper\" or \".loc\""},
                {"limit", "integer", "Most files to show (default 100, at most 2000)"}}),
        false, [this](const Value& a) -> Result<std::string> {
            auto folder = find(a["folder"].asString(), true);
            if (!folder)
                return std::unexpected(folder.error());
            const std::string filter = toLower(a["filter"].asString());
            const std::size_t limit = number(a["limit"], 100, 2000);
            std::string out;
            std::size_t total = 0;
            for (const VfsNode* f : Vfs::collectFiles(**folder)) {
                if (!filter.empty() && toLower(f->virtualPath()).find(filter) == std::string::npos)
                    continue;
                if (++total <= limit)
                    out += std::format("{}  ({}{}{})\n", f->virtualPath(), formatSize(f->size()),
                                       typeOf(*f).empty() ? "" : ", ", typeOf(*f));
            }
            return std::format("{} file(s){}\n{}", total, total > limit ? std::format(" (first {} shown)", limit) : "",
                               out);
        });

    add("read_file", "Read a file",
        "A file as text, with line numbers: game data tables, text tables and audio entities decompiled; text "
        "files as they are. Big files come in pages (start_line, max_lines). For binary files it says what they "
        "are and shows their first bytes.",
        schema({{"path", "string", "The file's path (from list_files)"},
                {"start_line", "integer", "First line to show (default 1)"},
                {"max_lines", "integer", "Lines to show (default 200, at most 2000)"}},
               {"path"}),
        false, [this](const Value& a) -> Result<std::string> {
            auto f = find(a["path"].asString());
            if (!f)
                return std::unexpected(f.error());
            auto text = textOf(**f);
            if (!text)
                return std::unexpected(text.error());
            if (!*text) {
                auto data = vfs->read(**f, 256);
                std::string hex;
                for (std::size_t i = 0; data && i < data->size() && i < 256; ++i)
                    hex += std::format("{:02x}{}", std::to_integer<unsigned>((*data)[i]), i % 32 == 31 ? "\n" : " ");
                const bool model = content.findMeshDecoder(extensionOf((*f)->name()), data ? *data : ByteBuffer{});
                return std::format("{}: {} ({}), {} bytes, not text.{}\nFirst bytes:\n{}", (*f)->virtualPath(),
                                   typeOf(**f).empty() ? "binary file" : typeOf(**f), extensionOf((*f)->name()),
                                   (*f)->size(), model ? " Use model_info to describe it." : "", hex);
            }
            const std::size_t first = number(a["start_line"], 1, SIZE_MAX);
            const std::size_t count = number(a["max_lines"], 200, 2000);
            std::string out;
            std::size_t line = 0, total = 0;
            for (const auto part : std::views::split(**text, '\n')) {
                ++total;
                if (++line >= first && line < first + count)
                    out += std::format("{:>6}| {}\n", line, std::string_view(part.begin(), part.end()));
            }
            const std::size_t last = std::min(first + count - 1, total);
            return clipped(std::format("{} — lines {}-{} of {}{}\n{}", (*f)->virtualPath(), first, last, total,
                                       last < total ? " (more: raise start_line)" : "", out));
        });

    add("search_text", "Search text",
        "Finds a text (case-insensitive) in the readable form of many files at once — decompiled game data, "
        "text tables, audio entities, text files — and lists file:line: matching line. Narrow it with `folder` "
        "and `filter` (e.g. folder \"cache/gamedata.pak\", filter \".bin\").",
        schema({{"query", "string", "Text to find, e.g. \"Viper\" or \"veh_Glass\""},
                {"folder", "string", "Only files under this folder"},
                {"filter", "string", "Only files whose path contains this, e.g. \".loc\""},
                {"max_results", "integer", "Most matching lines (default 100, at most 1000)"}},
               {"query"}),
        false, [this](const Value& a) -> Result<std::string> {
            const std::string query = toLower(a["query"].asString());
            if (query.empty())
                return fail(ErrorCode::InvalidFormat, "query is empty");
            auto folder = find(a["folder"].asString(), true);
            if (!folder)
                return std::unexpected(folder.error());
            const std::string filter = toLower(a["filter"].asString());
            const std::size_t most = number(a["max_results"], 100, 1000);
            std::string out;
            std::size_t hits = 0, searched = 0;
            for (const VfsNode* f : Vfs::collectFiles(**folder)) {
                if (!filter.empty() && toLower(f->virtualPath()).find(filter) == std::string::npos)
                    continue;
                const std::string ext = extensionOf(f->name());
                const FileTypeInfo* t = content.fileTypes().find(ext);
                if (t && (t->category == FileCategory::Texture || t->category == FileCategory::Audio
                          || t->category == FileCategory::Model || t->category == FileCategory::Video))
                    continue; // never text
                auto text = textOf(*f);
                if (!text || !*text)
                    continue;
                ++searched;
                std::size_t line = 0;
                for (const auto part : std::views::split(**text, '\n')) {
                    ++line;
                    const std::string_view l(part.begin(), part.end());
                    if (toLower(l).find(query) == std::string::npos)
                        continue;
                    if (++hits <= most)
                        out += std::format("{}:{}: {}\n", f->virtualPath(), line,
                                           l.size() > 200 ? std::string(l.substr(0, 200)) + "…" : std::string(l));
                }
            }
            return clipped(std::format("{} match(es) in {} file(s) searched{}\n{}", hits, searched,
                                       hits > most ? std::format(" (first {} shown)", most) : "", out));
        });

    if (content.hashTool())
        add("lookup", "Names and hashes",
            std::string(content.hashTool()->description()) +
                "\nAlso turns a text's English words into its name and id.",
            schema({{"query", "string", "A name, an English text, a path or a number (0x…)"}}, {"query"}), false,
            [this](const Value& a) -> Result<std::string> {
                const auto found = content.hashTool()->lookup(a["query"].asString(), vfs->hashToolContext());
                if (found.empty())
                    return std::string("nothing found");
                std::string out;
                for (const HashMatch& m : found)
                    out += std::format("{}: {}{}{}\n", m.kind, m.value, m.note.empty() ? "" : " — ", m.note);
                return out;
            });

    add("model_info", "Describe a model",
        "A 3D model's parts: the hierarchy of named elements, each part's element, the effect (material) it is "
        "drawn with, its triangles and the textures its material uses.",
        schema({{"path", "string", "The model's path, e.g. \"dodge_viperacr_stock.model\""}}, {"path"}), false,
        [this](const Value& a) -> Result<std::string> {
            auto f = find(a["path"].asString());
            if (!f)
                return std::unexpected(f.error());
            auto data = vfs->read(**f);
            if (!data)
                return std::unexpected(data.error());
            const MeshDecoder* d = content.findMeshDecoder(extensionOf((*f)->name()), *data);
            if (!d)
                return fail(ErrorCode::Unsupported, "{} isn't a model 26Viewer reads", (*f)->virtualPath());
            auto mesh = d->decodeWith(*data, contextFor(**f));
            if (!mesh)
                return std::unexpected(mesh.error());
            std::string out = std::format("{}: {} vertices, {} part(s), {} texture(s) stored in it\n",
                                          (*f)->virtualPath(), mesh->vertexCount(), mesh->parts.size(),
                                          mesh->textures.size());
            if (!mesh->nodes.empty()) {
                out += "\nElements (hierarchy):\n";
                std::function<void(int, int)> walk = [&](int parent, int depth) {
                    for (std::size_t i = 0; i < mesh->nodes.size(); ++i)
                        if (mesh->nodes[i].parent == parent && static_cast<int>(i) != parent) {
                            out += std::format("{}{}\n", std::string(2 + 2 * depth, ' '), mesh->nodes[i].name);
                            if (depth < 12)
                                walk(static_cast<int>(i), depth + 1);
                        }
                };
                walk(-1, 0);
            }
            out += "\nParts (element — effect — triangles at the most detailed level — textures):\n";
            for (const Mesh::Part& p : mesh->parts) {
                std::size_t tris = 0;
                for (const Mesh::Range& r : p.lods)
                    if (r.indexCount) {
                        tris = r.indexCount / 3;
                        break;
                    }
                std::string tex;
                for (const Mesh::TextureSlot& s : p.textures)
                    tex += std::format("{}{}={}", tex.empty() ? "" : ", ", s.name,
                                       s.texture >= 0 && static_cast<std::size_t>(s.texture) < mesh->textures.size()
                                           ? mesh->textures[static_cast<std::size_t>(s.texture)].name
                                           : std::string("(not in the file)"));
                out += std::format("  {} — {} — {}{}{}{}\n", p.name, p.shader.empty() ? p.material : p.shader, tris,
                                   p.shadow ? " (shadow hull)" : p.collision ? " (collision)" : "",
                                   tex.empty() ? "" : " — ", tex);
            }
            return clipped(out);
        });

    add("extract", "Extract files",
        "Copies files out of the game into a folder on disk, keeping their folders: the files given by `paths`, "
        "or every file under `folder` whose path contains `filter`.",
        schema({{"out_dir", "string", "Folder to write into (made if missing)"},
                {"paths", "string[]", "Files to extract"},
                {"folder", "string", "Or: extract everything under this folder…"},
                {"filter", "string", "…whose path contains this"}},
               {"out_dir"}),
        false, [this](const Value& a) -> Result<std::string> {
            const fs::path outDir = pathOf(a["out_dir"].asString());
            if (outDir.empty())
                return fail(ErrorCode::InvalidFormat, "out_dir is empty");
            std::vector<const VfsNode*> files;
            for (const Value& p : a["paths"].asArray()) {
                auto f = find(p.asString());
                if (!f)
                    return std::unexpected(f.error());
                files.push_back(*f);
            }
            if (files.empty()) {
                auto folder = find(a["folder"].asString(), true);
                if (!folder)
                    return std::unexpected(folder.error());
                const std::string filter = toLower(a["filter"].asString());
                for (const VfsNode* f : Vfs::collectFiles(**folder))
                    if (filter.empty() || toLower(f->virtualPath()).find(filter) != std::string::npos)
                        files.push_back(f);
            }
            if (files.size() > 5000)
                return fail(ErrorCode::InvalidFormat, "{} files: narrow it down (at most 5000 at once)", files.size());
            std::size_t done = 0;
            std::string problems;
            for (const VfsNode* f : files) {
                const fs::path dest = outDir / sanitizeRelativePath(f->virtualPath());
                auto data = vfs->read(*f);
                auto ok = data ? writeFile(dest, *data) : Result<void>(std::unexpected(data.error()));
                if (ok)
                    ++done;
                else
                    problems += std::format("\n  {}: {}", f->virtualPath(), ok.error().message);
            }
            return std::format("extracted {} file(s) into {}{}", done, utf8(outDir), problems);
        });

    if (!options.allowEdits)
        return;

    add("backup", "Back up an archive",
        "Keeps a copy of the archive (or loose file) holding `path` in the game's backup folder, once — an "
        "existing backup is the true original and is never overwritten. Needed before edit_text or replace_file "
        "write to it.",
        schema({{"path", "string", "Any file in the archive to back up"}}, {"path"}), true,
        [this](const Value& a) -> Result<std::string> {
            auto f = find(a["path"].asString());
            if (!f)
                return std::unexpected(f.error());
            const fs::path target = Vfs::editTarget(**f);
            const bool existed = !findBackup(target, vfs->backupRoot()).empty();
            auto backup = ensureBackup(target, vfs->backupRoot());
            if (!backup)
                return std::unexpected(backup.error());
            return std::format("{} {}", existed ? "a backup already exists:" : "backed up to", utf8(*backup));
        });

    add("edit_text", "Edit a file's text",
        "Changes a file through its readable form (what read_file shows): replaces `find` with `replace` "
        "— exactly one place unless `all` — and writes the file back (decompiled files are compiled back; only "
        "values can change, as their header says). Use read_file or search_text first to copy the exact text.",
        schema({{"path", "string", "The file"},
                {"find", "string", "Exact text to change (copy it from read_file, without the line numbers)"},
                {"replace", "string", "What to put instead"},
                {"all", "boolean", "Change every place it occurs (default: it must occur once)"}},
               {"path", "find", "replace"}),
        true, [this](const Value& a) -> Result<std::string> {
            auto f = find(a["path"].asString());
            if (!f)
                return std::unexpected(f.error());
            const std::string from = a["find"].asString(), to = a["replace"].asString();
            if (from.empty())
                return fail(ErrorCode::InvalidFormat, "find is empty");
            auto text = textOf(**f);
            if (!text)
                return std::unexpected(text.error());
            if (!*text)
                return fail(ErrorCode::Unsupported, "{} isn't text (use replace_file)", (*f)->virtualPath());
            std::string edited = **text;
            std::vector<std::size_t> at;
            for (std::size_t p = edited.find(from); p != std::string::npos; p = edited.find(from, p + 1))
                at.push_back(p);
            const auto lineOf = [&](std::size_t p) { return 1 + std::count(edited.begin(), edited.begin() + p, '\n'); };
            if (at.empty())
                return fail(ErrorCode::NotFound, "'{}' isn't in {} (check spaces and case with read_file)", from,
                            (*f)->virtualPath());
            if (at.size() > 1 && !a["all"].asBool()) {
                std::string lines;
                for (std::size_t i = 0; i < at.size() && i < 20; ++i)
                    lines += std::format(" {}", lineOf(at[i]));
                return fail(ErrorCode::InvalidFormat,
                            "'{}' is in {} places (lines{}); include more of the line to pick one, or set all", from,
                            at.size(), lines);
            }
            std::string changed;
            for (auto it = at.rbegin(); it != at.rend(); ++it) {
                changed = std::format("line {}\n", lineOf(*it)) + changed;
                edited.replace(*it, from.size(), to);
            }
            auto data = vfs->read(**f);
            if (!data)
                return std::unexpected(data.error());
            ByteBuffer bytes;
            if (const TextDecompiler* d = content.findDecompiler(extensionOf((*f)->name()), *data)) {
                if (!d->canRecompile())
                    return fail(ErrorCode::Unsupported, "{} can't be edited as text", (*f)->virtualPath());
                auto compiled = d->recompile(*data, edited, contextFor(**f));
                if (!compiled)
                    return std::unexpected(compiled.error());
                bytes = std::move(*compiled);
            } else {
                auto doc = decodeText(*data);
                if (!doc)
                    return std::unexpected(doc.error());
                doc->text = edited;
                auto encoded = encodeText(*doc);
                if (!encoded)
                    return std::unexpected(encoded.error());
                bytes = std::move(*encoded);
            }
            if (bytes == *data)
                return std::format("no change: {} stays as it is", (*f)->virtualPath());
            auto written = write(**f, bytes);
            if (!written)
                return std::unexpected(written.error());
            return std::format("{}\nchanged {} place(s): {}", *written, at.size(), changed);
        });

    add("replace_file", "Replace a file",
        "Replaces a file in the game with one from disk (same type, e.g. a .model exported from Blender), keeping "
        "its name. A .txt in the form read_file shows is compiled back into a data file.",
        schema({{"path", "string", "The file in the game to replace"},
                {"source", "string", "The file on disk to put there"}},
               {"path", "source"}),
        true, [this](const Value& a) -> Result<std::string> {
            auto f = find(a["path"].asString());
            if (!f)
                return std::unexpected(f.error());
            const fs::path source = pathOf(a["source"].asString());
            auto data = readFile(source);
            if (!data)
                return std::unexpected(data.error());
            const std::string have = extensionOf((*f)->name()), given = extensionOf(source);
            if (given == "txt" && have != "txt") {
                auto original = vfs->read(**f);
                if (!original)
                    return std::unexpected(original.error());
                const TextDecompiler* d = content.findDecompiler(have, *original);
                if (!d || !d->canRecompile())
                    return fail(ErrorCode::Unsupported, "{} can't be made from text", (*f)->virtualPath());
                auto compiled = d->recompile(
                    *original, std::string_view(reinterpret_cast<const char*>(data->data()), data->size()),
                    contextFor(**f));
                if (!compiled)
                    return std::unexpected(compiled.error());
                data = std::move(*compiled);
            } else if (given != have) {
                return fail(ErrorCode::InvalidFormat, "file types differ (.{} in the game, .{} given)", have, given);
            }
            return write(**f, *data);
        });
}

Server::Server(Options options) : m(std::make_unique<State>(std::move(options))) {}

Server::~Server() = default;

std::optional<std::string> Server::handle(std::string_view message)
{
    const auto reply = [](const Value& id, Value result) {
        Value r;
        r.set("jsonrpc", "2.0");
        r.set("id", id);
        r.set("result", std::move(result));
        return json::write(r);
    };
    const auto error = [](const Value& id, int code, std::string text) {
        Value e;
        e.set("code", code);
        e.set("message", std::move(text));
        Value r;
        r.set("jsonrpc", "2.0");
        r.set("id", id);
        r.set("error", std::move(e));
        return json::write(r);
    };

    auto parsed = json::parse(message);
    if (!parsed)
        return error(nullptr, -32700, parsed.error().message);
    const Value& msg = *parsed;
    if (!msg.isObject())
        return error(nullptr, -32600, "a JSON-RPC request object was expected");
    const Value& id = msg["id"];
    const std::string method = msg["method"].asString();
    const bool notification = id.isNull() && msg.asObject().end() == std::ranges::find_if(msg.asObject(), [](const auto& kv) {
                                  return kv.first == "id";
                              });
    if (method.empty())
        return notification ? std::nullopt : std::optional(error(id, -32600, "no method"));
    if (notification)
        return std::nullopt; // notifications/initialized, notifications/cancelled…
    const Value& params = msg["params"];

    if (method == "initialize") {
        const std::string asked = params["protocolVersion"].asString();
        std::string_view version = kVersions[0];
        for (std::string_view v : kVersions)
            if (v == asked)
                version = v;
        Value result;
        result.set("protocolVersion", version);
        Value caps;
        caps.set("tools", Object{{"listChanged", false}});
        result.set("capabilities", std::move(caps));
        result.set("serverInfo", Object{{"name", "26viewer"}, {"title", "26Viewer"}, {"version", m->options.version}});
        result.set("instructions",
                   std::format("26Viewer opens game files ({}). Call overview first, then list_files / search_text "
                               "to find things, read_file to read decompiled game data and texts, lookup for text "
                               "names, ids and path hashes, model_info for 3D models. {}",
                               m->game ? m->game->info().title : "no game recognised",
                               m->options.allowEdits
                                   ? "Edits are on: back up an archive (backup) before edit_text or replace_file "
                                     "write to it, and tell the user what you changed."
                                   : "Edits are off: the server only reads (start it with --allow-edits to change "
                                     "files)."));
        return reply(id, std::move(result));
    }
    if (method == "ping")
        return reply(id, Object{});
    if (method == "tools/list") {
        Array list;
        for (const State::Tool& t : m->tools) {
            Value tool;
            tool.set("name", t.name);
            tool.set("title", t.title);
            tool.set("description", t.description);
            tool.set("inputSchema", t.inputSchema);
            Value hints;
            hints.set("title", t.title);
            hints.set("readOnlyHint", !t.edits);
            hints.set("destructiveHint", t.edits && t.name != "backup");
            hints.set("openWorldHint", false);
            tool.set("annotations", std::move(hints));
            list.push_back(std::move(tool));
        }
        return reply(id, Object{{"tools", std::move(list)}});
    }
    if (method == "tools/call") {
        const std::string name = params["name"].asString();
        const auto tool = std::ranges::find(m->tools, name, &State::Tool::name);
        if (tool == m->tools.end())
            return error(id, -32602, std::format("unknown tool '{}'", name));
        Result<std::string> out = fail(ErrorCode::Unsupported, "not run");
        try {
            out = tool->run(params["arguments"]);
        } catch (const std::exception& e) {
            out = fail(ErrorCode::Io, "{}", e.what());
        }
        Value text;
        text.set("type", "text");
        text.set("text", out ? *out : out.error().message);
        Value result;
        result.set("content", Array{std::move(text)});
        result.set("isError", !out.has_value());
        return reply(id, std::move(result));
    }
    return error(id, -32601, std::format("unknown method '{}'", method));
}

void Server::run(std::istream& in, std::ostream& out)
{
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;
        if (auto r = handle(line)) {
            out << *r << '\n';
            out.flush();
        }
    }
}

} // namespace viewer::mcp
