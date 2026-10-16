#include "HashTool.hpp"

#include "Localisation.hpp"
#include "PakArchive.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <unordered_set>

namespace viewer::games::blur {

namespace {

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

/// 0x1234abcd, 1234abcd (eight hex digits) or a decimal number.
std::optional<std::uint32_t> number(std::string_view s)
{
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    } else if (s.size() == 8 && std::ranges::all_of(s, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })
               && !std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; })) {
        base = 16;
    }
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc() || end != s.data() + s.size() || s.empty())
        return std::nullopt;
    return value;
}

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

/// The archive's form of a path: "\models\a.model".
std::string storedPath(std::string_view path)
{
    std::string out(path);
    std::ranges::replace(out, '/', '\\');
    if (out.empty() || out.front() != '\\')
        out.insert(out.begin(), '\\');
    return out;
}

} // namespace

std::string_view BlurHashTool::description() const noexcept
{
    return "Texts: type a text's name (VEH_MOD_VIPERACR), its English text (Viper ACR) or its id (0xd2266d99); "
           "names come from the text tables in gamedata/xt/loc. Blur never computes a text's id from its name, so "
           "a new name gets the id 26Viewer gives new texts.\n"
           "Archive paths: type a path (models/vehicles/a.model) for its .pak name hash, or a hash to find its file.";
}

std::vector<HashMatch> BlurHashTool::lookup(std::string_view query, const HashToolContext& context) const
{
    const std::string_view q = trimmed(query);
    std::vector<HashMatch> out;
    if (q.empty())
        return out;
    const LocIndex texts = context.readFolder ? locIndex(context.readFolder("gamedata/xt/loc")) : LocIndex{};
    const auto textNote = [&](std::uint32_t id) {
        std::string note;
        if (const auto it = texts.english.find(id); it != texts.english.end())
            note = quotedText(it->second);
        if (const auto it = texts.files.find(id); it != texts.files.end())
            note += (note.empty() ? "in " : " in ") + it->second;
        return note;
    };
    std::vector<std::string> paths;
    bool pathsRead = false;
    const auto archivePaths = [&]() -> const std::vector<std::string>& {
        if (!pathsRead && context.archivePaths)
            paths = context.archivePaths();
        pathsRead = true;
        return paths;
    };

    // A number: the names behind it.
    if (const auto n = number(q)) {
        if (const auto it = texts.names.find(*n); it != texts.names.end())
            out.push_back({"Text", it->second, textNote(*n)});
        else if (texts.files.contains(*n))
            out.push_back({"Text", "(no name)", textNote(*n)});
        for (const std::string& path : archivePaths())
            if (PakArchive::nameHash(storedPath(path)) == *n)
                out.push_back({"Archive path", path, "its .pak name hash"});
        if (out.empty())
            out.push_back({"Not found", std::format("0x{:08x}", *n),
                           "no text or archive path in the open game has this number"});
    }

    // A name or a text: the numbers for it.
    if (const auto it = texts.ids.find(std::string(q)); it != texts.ids.end()) {
        out.push_back({"Text id", std::format("0x{:08x}", it->second), textNote(it->second)});
    } else if (isTextName(q) && !number(q)) {
        std::unordered_set<std::uint32_t> used;
        for (const auto& [id, file] : texts.files)
            used.insert(id);
        out.push_back({"New text id", std::format("0x{:08x}", newTextId(q, used)),
                       "no text has this name: the id 26Viewer gives a new text called this in a .loc table"});
    }
    const std::string wanted = lower(q);
    std::size_t shown = 0;
    for (const auto& [id, english] : texts.english) {
        if (lower(english) != wanted)
            continue;
        if (++shown > 20)
            break;
        const auto name = texts.names.find(id);
        out.push_back({"Text with this English", std::format("0x{:08x}", id),
                       (name != texts.names.end() ? name->second : std::string("(no name)")) + " " + textNote(id)});
    }
    if (q.find_first_of("/\\.") != std::string_view::npos) {
        const std::string stored = storedPath(q);
        const std::uint32_t hash = PakArchive::nameHash(stored);
        const bool there = std::ranges::any_of(archivePaths(), [&](const std::string& p) {
            return lower(storedPath(p)) == lower(stored);
        });
        out.push_back({"Archive path hash", std::format("0x{:08x}", hash),
                       there ? std::format("{} is in the open archives", stored)
                             : std::format("for {} (no such file in the open archives)", stored)});
    }
    return out;
}

} // namespace viewer::games::blur
