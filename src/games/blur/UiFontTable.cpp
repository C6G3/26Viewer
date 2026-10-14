// Blur's font tables (ui/data/ui_font_eur.dat; little-endian):
//
//   i32 -1, u32 fonts, then per font:
//     char name[128], u32 0, char texture[128] (the atlas, in ui_font_eur.bpr)
//     u32 atlas width, u32 atlas height, u32 slots, u32 line height, u32 0, u32 ?,
//     f32 1, f32 1
//     slots x 28 bytes: u32 character (0: unused slot, or the first: the fallback),
//       f32 u0, v0, u1, v1 (the character's rectangle in the atlas, 0..1),
//       i16 x offset, i16 width in pixels, i16 advance, u16 0
//     u32 pairs, pairs x (u16 left, u16 right, i8 kerning, u8 unused)
// The game's table reads to its last byte (3 fonts).

#include "UiFontTable.hpp"
#include "Localisation.hpp"

#include "viewer/core/TextEdit.hpp"

#include <cmath>
#include <cstring>
#include <format>

namespace viewer::games::blur {

namespace {

std::string fixedString(ByteSpan d, std::size_t at, std::size_t size)
{
    std::string s;
    for (std::size_t i = at; i < at + size && i < d.size() && d[i] != std::byte{0}; ++i)
        s.push_back(static_cast<char>(d[i]));
    return s;
}

/// A character as text: itself when printable ASCII, else U+XXXX.
std::string describe(char32_t c)
{
    if (c > 0x20 && c < 0x7F)
        return std::format("'{}'    ", static_cast<char>(c));
    return std::format("U+{:04X} ", static_cast<unsigned>(c));
}

/// An editable value of the text.
struct FontSlot {
    std::size_t offset;
    char kind;              ///< 's' name (128 bytes), 'U' u32, 'x' 'y' 'w' 'h' atlas pixels, 'i' i16, 'k' i8
    std::uint32_t size = 0; ///< Atlas width (x, w) or height (y, h).
};

Result<std::string> decompileFonts(ByteSpan d, std::vector<FontSlot>* edits);

} // namespace

bool UiFontTableDecompiler::canDecompile(std::string_view extension, ByteSpan data) const
{
    return extension == "dat" && data.size() >= 8 && loadScalar<std::int32_t>(data, 0) == -1
           && loadScalar<std::uint32_t>(data, 4) > 0 && loadScalar<std::uint32_t>(data, 4) < 64;
}

Result<std::string> UiFontTableDecompiler::decompile(ByteSpan d) const
{
    return decompileFonts(d, nullptr);
}

namespace {

Result<std::string> decompileFonts(ByteSpan d, std::vector<FontSlot>* edits)
{
    const auto editable = [&](std::string value, std::size_t offset, char kind, std::uint32_t size = 0) {
        if (!edits)
            return value;
        edits->push_back({offset, kind, size});
        return EditableText::mark(value, edits->size() - 1);
    };
    if (!UiFontTableDecompiler().canDecompile("dat", d))
        return fail(ErrorCode::InvalidFormat, "not a Blur font table");
    const std::uint32_t fonts = loadScalar<std::uint32_t>(d, 4);
    std::string out = std::format("// Blur font table: {} font(s) drawn from atlas textures (ui_font_eur.bpr)\n", fonts);
    std::size_t p = 8;
    const auto ends = [&](std::size_t n) {
        return fail(ErrorCode::InvalidFormat, "font table ends early (byte {}, {} more needed)", p, n);
    };
    for (std::uint32_t f = 0; f < fonts; ++f) {
        if (p + 292 > d.size())
            return ends(292);
        const std::string name = fixedString(d, p, 128), texture = fixedString(d, p + 132, 128);
        const std::uint32_t width = loadScalar<std::uint32_t>(d, p + 260), height = loadScalar<std::uint32_t>(d, p + 264),
                            slots = loadScalar<std::uint32_t>(d, p + 268), line = loadScalar<std::uint32_t>(d, p + 272);
        p += 292;
        if (slots > 65536 || p + std::size_t{slots} * 28 + 4 > d.size())
            return ends(std::size_t{slots} * 28 + 4);
        out += std::format("\nfont {}\n{{\n    texture      {}   // {} x {} atlas\n    line_height  {}\n\n",
                           editable(quotedText(name), p - 292, 's'), editable(quotedText(texture), p - 292 + 132, 's'),
                           width, height, editable(std::to_string(line), p - 292 + 272, 'U'));
        out += "    //  char      atlas x  y    w   h     x_offset  width  advance\n";
        std::size_t used = 0;
        for (std::uint32_t i = 0; i < slots; ++i, p += 28) {
            const char32_t c = loadScalar<std::uint32_t>(d, p);
            const float u0 = loadScalar<float>(d, p + 4), v0 = loadScalar<float>(d, p + 8),
                        u1 = loadScalar<float>(d, p + 12), v1 = loadScalar<float>(d, p + 16);
            if (c == 0 && i > 0)
                continue; // an unused slot
            ++used;
            const auto px = [](float t, std::uint32_t size) { return static_cast<long>(std::lround(t * static_cast<float>(size))); };
            const auto number = [&](long v, std::size_t width4, std::size_t at, char kind, std::uint32_t size = 0) {
                std::string s = std::to_string(v);
                const std::string pad(s.size() < width4 ? width4 - s.size() : 0, ' ');
                return pad + editable(std::move(s), at, kind, size);
            };
            out += std::format("    char {} at {} {} {} {}   {}      {}   {}{}\n", describe(c),
                               number(px(u0, width), 4, p + 4, 'x', width), number(px(v0, height), 4, p + 8, 'y', height),
                               number(px(u1 - u0, width), 4, p + 4, 'w', width),
                               number(px(v1 - v0, height), 3, p + 8, 'h', height),
                               number(loadScalar<std::int16_t>(d, p + 20), 4, p + 20, 'i'),
                               number(loadScalar<std::int16_t>(d, p + 22), 4, p + 22, 'i'),
                               number(loadScalar<std::int16_t>(d, p + 24), 4, p + 24, 'i'), i == 0 ? "   // fallback" : "");
        }
        const std::uint32_t pairs = loadScalar<std::uint32_t>(d, p);
        p += 4;
        if (p + std::size_t{pairs} * 6 > d.size())
            return ends(std::size_t{pairs} * 6);
        out += std::format("\n    // {} character(s); kerning: {} pair(s)\n", used, pairs);
        for (std::uint32_t i = 0; i < pairs; ++i, p += 6)
            out += std::format("    kern {} {} {}\n", describe(loadScalar<std::uint16_t>(d, p)),
                               describe(loadScalar<std::uint16_t>(d, p + 2)),
                               editable(std::to_string(static_cast<int>(loadScalar<std::int8_t>(d, p + 4))), p + 4, 'k'));
        out += "}\n";
    }
    if (p != d.size())
        out += std::format("\n// {} byte(s) after the last font not read\n", d.size() - p);
    return out;
}

} // namespace

Result<ByteBuffer> UiFontTableDecompiler::recompile(ByteSpan original, std::string_view text, const DecompileContext&) const
{
    std::vector<FontSlot> slots;
    auto marked = decompileFonts(original, &slots);
    if (!marked)
        return std::unexpected(marked.error());
    std::vector<std::size_t> ids;
    const EditableText before = EditableText::finish(*marked, &ids);
    auto changes = changedValues(before, text);
    if (!changes)
        return std::unexpected(changes.error());
    ByteBuffer out(original.begin(), original.end());
    for (const ChangedValue& c : *changes) {
        const FontSlot& slot = slots[ids[c.index]];
        const auto bad = [&](std::string_view why) {
            return fail(ErrorCode::InvalidFormat, "line {}: \"{}\" {}", c.line, c.text, why);
        };
        if (slot.kind == 's') {
            const auto s = unquote(c.text);
            if (!s || s->size() > 127 || s->find('\0') != std::string::npos)
                return bad("isn't a name in double quotes of at most 127 characters");
            std::memset(out.data() + slot.offset, 0, 128);
            std::memcpy(out.data() + slot.offset, s->data(), s->size());
            continue;
        }
        const auto v = parseInteger(c.text);
        const auto range = [&](long long lo, long long hi) {
            return bad(std::format("isn't a whole number from {} to {}", lo, hi));
        };
        switch (slot.kind) {
        case 'U':
            if (!v || *v < 0 || *v > 0xFFFFFFFFll)
                return range(0, 0xFFFFFFFFll);
            storeScalar(out, slot.offset, static_cast<std::uint32_t>(*v));
            break;
        case 'i':
            if (!v || *v < -32768 || *v > 32767)
                return range(-32768, 32767);
            storeScalar(out, slot.offset, static_cast<std::int16_t>(*v));
            break;
        case 'k':
            if (!v || *v < -128 || *v > 127)
                return range(-128, 127);
            storeScalar(out, slot.offset, static_cast<std::int8_t>(*v));
            break;
        default: { // atlas pixels: x/y move the rectangle, w/h resize it
            if (!v || *v < 0 || *v > slot.size)
                return range(0, slot.size);
            const float start = loadScalar<float>(out, slot.offset), end = loadScalar<float>(out, slot.offset + 8);
            const float value = static_cast<float>(*v) / static_cast<float>(slot.size);
            if (slot.kind == 'x' || slot.kind == 'y') {
                storeScalar(out, slot.offset, value);
                storeScalar(out, slot.offset + 8, value + (end - start));
            } else {
                storeScalar(out, slot.offset + 8, start + value);
            }
            break;
        }
        }
    }
    return out;
}

} // namespace viewer::games::blur
