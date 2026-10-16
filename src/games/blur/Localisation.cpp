// Blur's text tables (gamedata/xt/loc/*.loc; little-endian):
//
//   0x00 "OLTX", u32 version 1, u32 16, u32 (first language table - 16)
//   0x10 u32 12, u32 languages, u32 (key table - 16)
//   0x1C languages x {char code[16] ("en"), u32 table offset, u32 table size}
//   then the key table and one table per language, each:
//     u32 count, u32 8, count x {u32 id, u32 string offset from the table start},
//     then the strings: 8-bit names in the key table ("VEH_FULL_ASTRA_LEGEND"),
//     UTF-16 texts in the language tables; all end with a 0.
// The ids are what data tables store for a text (a "LocId").
//
// The name table is sorted by name and each language's table by id (every
// shipped table): Blur.exe 0x73EA90 finds a text by name with a binary search
// (then the name + "_PC"), and returns the id stored next to it. The game never
// computes an id from a name, so a new text can take any unused id.

#include "Localisation.hpp"

#include "viewer/core/Compression.hpp"
#include "viewer/core/TextEdit.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <ranges>

namespace viewer::games::blur {

namespace {

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

/// One id table: id -> text.
Result<std::vector<std::pair<std::uint32_t, std::string>>> readTable(ByteSpan d, std::size_t at, bool wide)
{
    if (at + 8 > d.size())
        return fail(ErrorCode::InvalidFormat, "text table at {} is past the end", at);
    const std::uint32_t count = loadScalar<std::uint32_t>(d, at);
    if (loadScalar<std::uint32_t>(d, at + 4) != 8 || count > (d.size() - at - 8) / 8)
        return fail(ErrorCode::InvalidFormat, "text table at {} is damaged", at);
    std::vector<std::pair<std::uint32_t, std::string>> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t id = loadScalar<std::uint32_t>(d, at + 8 + 8 * i);
        std::size_t p = at + loadScalar<std::uint32_t>(d, at + 12 + 8 * i);
        std::string text;
        if (wide) {
            for (; p + 2 <= d.size(); p += 2) {
                char32_t c = loadScalar<std::uint16_t>(d, p);
                if (c == 0)
                    break;
                if (c >= 0xD800 && c < 0xDC00 && p + 4 <= d.size()) {
                    const char32_t low = loadScalar<std::uint16_t>(d, p + 2);
                    if (low >= 0xDC00 && low < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                        p += 2;
                    }
                }
                appendUtf8(text, c);
            }
        } else {
            for (; p < d.size() && d[p] != std::byte{0}; ++p)
                text.push_back(static_cast<char>(d[p]));
        }
        out.emplace_back(id, std::move(text));
    }
    return out;
}

/// UTF-8 to UTF-16 (little-endian bytes, with the closing 0).
std::optional<ByteBuffer> utf16(std::string_view s)
{
    ByteBuffer out;
    const auto unit = [&](std::uint32_t u) {
        out.push_back(static_cast<std::byte>(u & 0xFF));
        out.push_back(static_cast<std::byte>(u >> 8));
    };
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!length || i + length > s.size())
            return std::nullopt;
        std::uint32_t cp = length == 1 ? c : c & (0x7F >> length);
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(s[i + k]);
            if ((next & 0xC0) != 0x80)
                return std::nullopt;
            cp = cp << 6 | (next & 0x3F);
        }
        i += length;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    unit(0);
    return out;
}

} // namespace

std::string quotedText(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\x{:02x}", static_cast<unsigned>(c));
            else
                out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

Result<LocTable> readLocTable(ByteSpan d)
{
    if (d.size() < 0x1C || loadScalar<std::uint32_t>(d, 0) != 0x58544C4F) // "OLTX"
        return fail(ErrorCode::InvalidFormat, "not a Blur text table");
    if (loadScalar<std::uint32_t>(d, 4) != 1)
        return fail(ErrorCode::Unsupported, "text table version {} isn't supported", loadScalar<std::uint32_t>(d, 4));
    const std::uint32_t languages = loadScalar<std::uint32_t>(d, 0x14);
    if (languages > 64 || 0x1C + std::size_t{languages} * 24 > d.size())
        return fail(ErrorCode::InvalidFormat, "text table header is damaged");
    LocTable table;
    auto keys = readTable(d, 0x10 + std::size_t{loadScalar<std::uint32_t>(d, 0x18)}, false);
    if (!keys)
        return std::unexpected(keys.error());
    std::unordered_map<std::uint32_t, std::size_t> index;
    for (auto& [id, key] : *keys) {
        index.emplace(id, table.strings.size());
        table.strings.push_back({id, std::move(key), std::vector<std::string>(languages)});
    }
    for (std::uint32_t l = 0; l < languages; ++l) {
        const std::size_t entry = 0x1C + std::size_t{l} * 24;
        std::string code;
        for (std::size_t i = entry; i < entry + 16 && d[i] != std::byte{0}; ++i)
            code.push_back(static_cast<char>(d[i]));
        table.languages.push_back(std::move(code));
        auto texts = readTable(d, loadScalar<std::uint32_t>(d, entry + 16), true);
        if (!texts)
            return std::unexpected(texts.error());
        for (auto& [id, text] : *texts) {
            auto it = index.find(id);
            if (it == index.end()) { // a text without a name
                it = index.emplace(id, table.strings.size()).first;
                table.strings.push_back({id, {}, std::vector<std::string>(languages)});
            }
            table.strings[it->second].texts[l] = std::move(text);
        }
    }
    return table;
}

LocIndex locIndex(const std::vector<DecompileContext::File>& files)
{
    LocIndex out;
    for (const auto& file : files) {
        auto table = readLocTable(file.data);
        if (!table)
            continue;
        std::size_t english = 0;
        for (std::size_t l = 0; l < table->languages.size(); ++l)
            if (table->languages[l] == "en")
                english = l;
        for (auto& s : table->strings) {
            out.files.emplace(s.id, file.name);
            if (!s.key.empty()) {
                out.names.emplace(s.id, s.key);
                out.ids.emplace(s.key, s.id);
            }
            if (english < s.texts.size() && !s.texts[english].empty())
                out.english.emplace(s.id, std::move(s.texts[english]));
        }
    }
    return out;
}

std::unordered_map<std::uint32_t, std::string> englishTexts(const std::vector<DecompileContext::File>& files)
{
    return locIndex(files).english;
}

std::uint32_t newTextId(std::string_view name, const std::unordered_set<std::uint32_t>& used)
{
    std::uint32_t id = compression::crc32(asBytes(name)) ^ 0xFFFFFFFFu;
    while (id == 0 || used.contains(id))
        ++id;
    return id;
}

bool isTextName(std::string_view name)
{
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

ByteBuffer writeLocTable(const LocTable& table)
{
    const std::size_t languages = table.languages.size();
    const std::size_t keyTable = 0x1C + 24 * languages;
    // count, 8, (id, offset from the table start) per entry, then the strings
    const auto writeTable = [](ByteBuffer& out, const std::vector<std::pair<std::uint32_t, ByteBuffer>>& entries) {
        const std::size_t start = out.size();
        const auto count = static_cast<std::uint32_t>(entries.size());
        out.resize(start + 8 + 8 * std::size_t{count});
        storeScalar(out, start, count);
        storeScalar(out, start + 4, std::uint32_t{8});
        for (std::size_t i = 0; i < entries.size(); ++i) {
            storeScalar(out, start + 8 + 8 * i, entries[i].first);
            storeScalar(out, start + 12 + 8 * i, static_cast<std::uint32_t>(out.size() - start));
            out.insert(out.end(), entries[i].second.begin(), entries[i].second.end());
        }
    };

    ByteBuffer out(keyTable);
    std::vector<const LocString*> named;
    for (const LocString& s : table.strings)
        if (!s.key.empty())
            named.push_back(&s);
    std::ranges::sort(named, {}, &LocString::key); // byte order, as the game compares
    std::vector<std::pair<std::uint32_t, ByteBuffer>> keys;
    for (const LocString* s : named) {
        ByteBuffer name(s->key.size() + 1);
        std::memcpy(name.data(), s->key.data(), s->key.size());
        keys.emplace_back(s->id, std::move(name));
    }
    writeTable(out, keys);

    std::vector<const LocString*> byId;
    for (const LocString& s : table.strings)
        byId.push_back(&s);
    std::ranges::sort(byId, {}, &LocString::id);
    std::vector<std::pair<std::size_t, std::size_t>> placed;
    for (std::size_t l = 0; l < languages; ++l) {
        std::vector<std::pair<std::uint32_t, ByteBuffer>> texts;
        for (const LocString* s : byId) {
            auto wide = utf16(l < s->texts.size() ? s->texts[l] : std::string());
            texts.emplace_back(s->id, wide ? std::move(*wide) : ByteBuffer{std::byte{0}, std::byte{0}});
        }
        const std::size_t at = out.size();
        writeTable(out, texts);
        placed.emplace_back(at, out.size() - at);
    }

    storeScalar(out, 0, std::uint32_t{0x58544C4F}); // "OLTX"
    storeScalar(out, 4, std::uint32_t{1});
    storeScalar(out, 8, std::uint32_t{16});
    storeScalar(out, 12, static_cast<std::uint32_t>((placed.empty() ? out.size() : placed[0].first) - 16));
    storeScalar(out, 0x10, std::uint32_t{12});
    storeScalar(out, 0x14, static_cast<std::uint32_t>(languages));
    storeScalar(out, 0x18, static_cast<std::uint32_t>(keyTable - 16));
    for (std::size_t l = 0; l < languages; ++l) {
        const std::size_t entry = 0x1C + 24 * l;
        const std::string& code = table.languages[l];
        std::memcpy(out.data() + entry, code.data(), std::min<std::size_t>(code.size(), 15));
        storeScalar(out, entry + 16, static_cast<std::uint32_t>(placed[l].first));
        storeScalar(out, entry + 20, static_cast<std::uint32_t>(placed[l].second));
    }
    return out;
}

bool LocTableDecompiler::canDecompile(std::string_view, ByteSpan data) const
{
    return data.size() >= 0x1C && loadScalar<std::uint32_t>(data, 0) == 0x58544C4F;
}

namespace {

/// The table as text; `slots` gets (language, text id) for each text, in order.
Result<EditableText> locText(ByteSpan data, std::vector<std::pair<std::size_t, std::uint32_t>>& slots)
{
    auto table = readLocTable(data);
    if (!table)
        return std::unexpected(table.error());
    std::string languages;
    for (const auto& l : table->languages)
        languages += (languages.empty() ? "" : ", ") + l;
    std::string out = std::format("// Blur text table: {} text(s) in {}\n", table->strings.size(), languages);
    for (const LocString& s : table->strings) {
        out += std::format("\n{}  // id 0x{:08x}\n", s.key.empty() ? std::string("(no name)") : s.key, s.id);
        for (std::size_t l = 0; l < s.texts.size(); ++l) {
            out += std::format("    {} {}\n", table->languages[l], EditableText::mark(quotedText(s.texts[l]), slots.size()));
            slots.emplace_back(l, s.id);
        }
    }
    return EditableText::finish(out);
}

} // namespace

Result<std::string> LocTableDecompiler::decompile(ByteSpan data) const
{
    std::vector<std::pair<std::size_t, std::uint32_t>> slots;
    auto text = locText(data, slots);
    if (!text)
        return std::unexpected(text.error());
    return std::move(text->text);
}

namespace {

/// One text as written in the edited listing.
struct WrittenText {
    std::string name;                                       ///< "" for "(no name)"
    std::optional<std::uint32_t> id;                        ///< From "// id 0x…"; none for a new text.
    std::vector<std::pair<std::string, std::string>> texts; ///< (language, text)
    std::size_t line = 0;
};

std::string_view trimmedLine(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

/// The listing read line by line: a name line ("NAME  // id 0x…"), then one
/// indented line per language (`en "text"`); blank lines and // comments skipped.
Result<std::vector<WrittenText>> readListing(std::string_view text)
{
    std::vector<WrittenText> out;
    std::size_t number = 0;
    for (const auto part : std::views::split(text, '\n')) {
        ++number;
        const std::string_view raw(part.begin(), part.end());
        const std::string_view line = trimmedLine(raw);
        if (line.empty() || line.starts_with("//"))
            continue;
        if (raw.front() == ' ' || raw.front() == '\t') {
            if (out.empty())
                return fail(ErrorCode::InvalidFormat, "line {}: a text before any name", number);
            const std::size_t space = line.find_first_of(" \t");
            if (space == std::string_view::npos)
                return fail(ErrorCode::InvalidFormat, "line {}: expected a language and a text in quotes", number);
            const auto value = unquote(trimmedLine(line.substr(space)));
            if (!value)
                return fail(ErrorCode::InvalidFormat, "line {}: {} isn't a text in double quotes", number,
                            trimmedLine(line.substr(space)));
            out.back().texts.emplace_back(std::string(line.substr(0, space)), *value);
            continue;
        }
        WrittenText t;
        t.line = number;
        const std::size_t comment = line.find("//");
        const std::string_view name = trimmedLine(line.substr(0, comment));
        t.name = name == "(no name)" ? std::string() : std::string(name);
        if (comment != std::string_view::npos) {
            const std::string_view note = line.substr(comment);
            if (const std::size_t at = note.find("id 0x"); at != std::string_view::npos) {
                std::uint32_t id = 0;
                const char* first = note.data() + at + 5;
                const auto [end, ec] = std::from_chars(first, note.data() + note.size(), id, 16);
                if (ec != std::errc() || end == first)
                    return fail(ErrorCode::InvalidFormat, "line {}: the id isn't a hex number", number);
                t.id = id;
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

/// The table with the listing's texts changed and its new texts added. `others`:
/// what the game's other text tables hold (a name they have keeps its id; new
/// ids avoid theirs).
Result<ByteBuffer> withNewTexts(ByteSpan d, std::string_view text, const LocIndex& others)
{
    auto table = readLocTable(d);
    if (!table)
        return std::unexpected(table.error());
    auto written = readListing(text);
    if (!written)
        return std::unexpected(written.error());
    const auto language = [&](const std::string& code) -> std::optional<std::size_t> {
        for (std::size_t l = 0; l < table->languages.size(); ++l)
            if (table->languages[l] == code)
                return l;
        return std::nullopt;
    };

    std::unordered_map<std::uint32_t, std::size_t> byId;
    std::unordered_set<std::string> names;
    std::unordered_set<std::uint32_t> used;
    for (std::size_t i = 0; i < table->strings.size(); ++i) {
        byId.emplace(table->strings[i].id, i);
        names.insert(table->strings[i].key);
        used.insert(table->strings[i].id);
    }
    for (const auto& [id, file] : others.files)
        used.insert(id);

    std::unordered_set<std::uint32_t> seen;
    const std::size_t existing = table->strings.size();
    for (const WrittenText& w : *written) {
        std::size_t index = 0;
        if (w.id && byId.contains(*w.id)) {
            index = byId.at(*w.id);
            if (table->strings[index].key != w.name)
                return fail(ErrorCode::InvalidFormat, "line {}: id 0x{:08x} is called {} (an existing text's name "
                            "can't change)", w.line, *w.id,
                            table->strings[index].key.empty() ? "(no name)" : table->strings[index].key);
        } else {
            // A new text
            if (!isTextName(w.name))
                return fail(ErrorCode::InvalidFormat, "line {}: \"{}\" can't name a text (letters, digits and _ only)",
                            w.line, w.name);
            if (names.contains(w.name))
                return fail(ErrorCode::InvalidFormat, "line {}: this table already has a text called {}", w.line, w.name);
            std::uint32_t id = 0;
            if (const auto it = others.ids.find(w.name); it != others.ids.end())
                id = it->second; // the same name in another table: the same text, the same id
            else if (w.id && !used.contains(*w.id) && *w.id != 0)
                id = *w.id;
            else if (w.id)
                return fail(ErrorCode::InvalidFormat, "line {}: id 0x{:08x} is taken (leave the id out and 26Viewer "
                            "picks a free one)", w.line, *w.id);
            else
                id = newTextId(w.name, used);
            used.insert(id);
            names.insert(w.name);
            index = table->strings.size();
            byId.emplace(id, index);
            table->strings.push_back({id, w.name, std::vector<std::string>(table->languages.size())});
        }
        if (!seen.insert(table->strings[index].id).second)
            return fail(ErrorCode::InvalidFormat, "line {}: {} is listed twice", w.line, w.name);
        LocString& s = table->strings[index];
        for (const auto& [code, value] : w.texts) {
            const auto l = language(code);
            if (!l)
                return fail(ErrorCode::InvalidFormat, "line {}: this table has no language \"{}\"", w.line, code);
            s.texts[*l] = value;
        }
        if (index >= existing) {
            // Languages left out take the first one given (the game shows something everywhere).
            for (std::string& t : s.texts)
                if (t.empty() && !w.texts.empty())
                    t = w.texts.front().second;
        }
    }
    for (std::size_t i = 0; i < existing; ++i)
        if (!seen.contains(table->strings[i].id))
            return fail(ErrorCode::InvalidFormat, "{} (id 0x{:08x}) is missing: texts can't be removed (the game may "
                        "use them); give it empty texts instead",
                        table->strings[i].key.empty() ? "(no name)" : table->strings[i].key, table->strings[i].id);
    ByteBuffer out = writeLocTable(*table);
    if (auto check = readLocTable(out); !check)
        return fail(ErrorCode::InvalidFormat, "the rebuilt text table doesn't read back: {}", check.error().message);
    return out;
}

} // namespace

Result<ByteBuffer> LocTableDecompiler::recompile(ByteSpan d, std::string_view text, const DecompileContext& context) const
{
    std::vector<std::pair<std::size_t, std::uint32_t>> slots;
    auto original = locText(d, slots);
    if (!original)
        return std::unexpected(original.error());
    auto changes = changedValues(*original, text);
    if (!changes) {
        // More than texts changed: new texts were added (or there's a mistake,
        // which the listing reader names).
        auto written = readListing(text);
        auto table = readLocTable(d);
        if (!written || !table)
            return std::unexpected(changes.error());
        // The same texts with the same names: a mistake in a value (changedValues says where).
        std::unordered_map<std::uint32_t, const std::string*> names;
        for (const LocString& s : table->strings)
            names.emplace(s.id, &s.key);
        const bool sameTexts =
            written->size() == table->strings.size() && std::ranges::all_of(*written, [&](const WrittenText& w) {
                const auto it = w.id ? names.find(*w.id) : names.end();
                return it != names.end() && *it->second == w.name;
            });
        if (sameTexts)
            return std::unexpected(changes.error());
        return withNewTexts(d, text, context.readFolder ? locIndex(context.readFolder("gamedata/xt/loc")) : LocIndex{});
    }
    if (changes->empty())
        return ByteBuffer(d.begin(), d.end());
    std::map<std::pair<std::size_t, std::uint32_t>, ByteBuffer> replacements;
    for (const ChangedValue& c : *changes) {
        const auto s = unquote(c.text);
        if (!s)
            return fail(ErrorCode::InvalidFormat, "line {}: {} isn't a text in double quotes", c.line, c.text);
        auto wide = utf16(*s);
        if (!wide)
            return fail(ErrorCode::InvalidFormat, "line {}: the text isn't valid UTF-8", c.line);
        replacements[slots[c.index]] = std::move(*wide);
    }

    // The language tables are rewritten in place of the old ones (they follow
    // the key table, one after another), each keeping its order.
    const std::uint32_t languages = loadScalar<std::uint32_t>(d, 0x14);
    std::vector<std::size_t> starts;
    for (std::uint32_t l = 0; l < languages; ++l)
        starts.push_back(loadScalar<std::uint32_t>(d, 0x1C + 24 * l + 16));
    for (std::size_t l = 1; l < starts.size(); ++l)
        if (starts[l] < starts[l - 1])
            return fail(ErrorCode::Unsupported, "this text table's languages aren't stored in order");
    if (starts.empty())
        return ByteBuffer(d.begin(), d.end());
    ByteBuffer out(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(starts[0]));
    for (std::uint32_t l = 0; l < languages; ++l) {
        const std::size_t at = starts[l];
        const std::uint32_t count = loadScalar<std::uint32_t>(d, at);
        const std::size_t tableStart = out.size();
        ByteBuffer strings;
        out.resize(out.size() + 8 + std::size_t{count} * 8);
        storeScalar(out, tableStart, count);
        storeScalar(out, tableStart + 4, std::uint32_t{8});
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t id = loadScalar<std::uint32_t>(d, at + 8 + 8 * i);
            storeScalar(out, tableStart + 8 + 8 * i, id);
            storeScalar(out, tableStart + 12 + 8 * i, static_cast<std::uint32_t>(8 + 8 * count + strings.size()));
            if (const auto it = replacements.find({l, id}); it != replacements.end()) {
                strings.insert(strings.end(), it->second.begin(), it->second.end());
                continue;
            }
            std::size_t p = at + loadScalar<std::uint32_t>(d, at + 12 + 8 * i);
            const std::size_t begin = p;
            while (p + 2 <= d.size() && (d[p] != std::byte{0} || d[p + 1] != std::byte{0}))
                p += 2;
            strings.insert(strings.end(), d.begin() + static_cast<std::ptrdiff_t>(begin),
                           d.begin() + static_cast<std::ptrdiff_t>(std::min(p + 2, d.size())));
        }
        out.insert(out.end(), strings.begin(), strings.end());
        storeScalar(out, 0x1C + 24 * l + 16, static_cast<std::uint32_t>(tableStart));
        storeScalar(out, 0x1C + 24 * l + 20, static_cast<std::uint32_t>(out.size() - tableStart));
    }
    // Anything after the last table stays.
    const std::size_t end = starts.back() + loadScalar<std::uint32_t>(d, 0x1C + 24 * (languages - 1) + 20);
    if (end < d.size())
        out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(end), d.end());
    if (auto check = readLocTable(out); !check)
        return fail(ErrorCode::InvalidFormat, "the rebuilt text table doesn't read back: {}", check.error().message);
    return out;
}

} // namespace viewer::games::blur
