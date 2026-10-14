// Blur's .pf fonts (little-endian).
//
//   0x000  the font's name (NUL-terminated); the rest of the 256-byte header is
//          the exporter's memory (a source folder, "FONT - <name>", fill bytes)
//   0x100  u32 glyphs, then per glyph:
//            u16 character (Unicode), f32 left space, f32 width, f32 right space,
//            u32 contours, per contour u32 points and per point
//            f32 x, f32 y, u8 kind: 0 on the outline (a line leads to it),
//            2 control point of a quadratic curve to the next point,
//            3 first of two control points of a cubic curve
//          x runs from 0 at the glyph's left edge; y points up (the baseline is
//          the bottom of the capitals, ~90 of a 480-high box)
//   then   u32 pairs, per pair u16 left, u16 right, f32 kerning
// Every .pf in the game reads to its last byte.

#include "FontFile.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace viewer::games::blur {

namespace {

constexpr std::size_t kGlyphTable = 0x100;

} // namespace

bool PfFontDecoder::canDecode(std::string_view extension, ByteSpan data) const
{
    return extension == "pf" && data.size() >= kGlyphTable + 4;
}

Result<Font> PfFontDecoder::decode(ByteSpan d) const
{
    if (d.size() < kGlyphTable + 4)
        return fail(ErrorCode::InvalidFormat, "too small for a font ({} bytes)", d.size());
    Font font;
    const auto nameEnd = std::find(d.begin(), d.begin() + 64, std::byte{0});
    font.name.assign(reinterpret_cast<const char*>(d.data()), static_cast<std::size_t>(nameEnd - d.begin()));

    std::size_t p = kGlyphTable;
    const auto has = [&](std::size_t n) { return n <= d.size() && p <= d.size() - n; };
    const auto damaged = [&] { return fail(ErrorCode::InvalidFormat, "font data ends early (byte {})", p); };
    const std::uint32_t count = loadScalar<std::uint32_t>(d, p);
    p += 4;
    if (count > 65536)
        return fail(ErrorCode::InvalidFormat, "{} glyphs is too many for a font", count);
    font.glyphs.reserve(count);
    for (std::uint32_t g = 0; g < count; ++g) {
        if (!has(18))
            return damaged();
        Font::Glyph glyph;
        glyph.code = loadScalar<std::uint16_t>(d, p);
        glyph.leftSpace = loadScalar<float>(d, p + 2);
        glyph.width = loadScalar<float>(d, p + 6);
        glyph.rightSpace = loadScalar<float>(d, p + 10);
        const std::uint32_t contours = loadScalar<std::uint32_t>(d, p + 14);
        p += 18;
        if (contours > 1024)
            return fail(ErrorCode::InvalidFormat, "glyph U+{:04X} has {} contours", static_cast<unsigned>(glyph.code),
                        contours);
        for (std::uint32_t c = 0; c < contours; ++c) {
            if (!has(4))
                return damaged();
            const std::uint32_t points = loadScalar<std::uint32_t>(d, p);
            p += 4;
            if (!has(std::size_t{points} * 9))
                return damaged();
            auto& contour = glyph.contours.emplace_back();
            contour.reserve(points);
            for (std::uint32_t i = 0; i < points; ++i, p += 9) {
                Font::Point point{loadScalar<float>(d, p), loadScalar<float>(d, p + 4), Font::Point::Kind::On};
                switch (std::to_integer<std::uint8_t>(d[p + 8])) {
                case 2: point.kind = Font::Point::Kind::Quadratic; break;
                case 3: point.kind = Font::Point::Kind::Cubic; break;
                default: break;
                }
                if (!std::isfinite(point.x) || !std::isfinite(point.y))
                    return fail(ErrorCode::InvalidFormat, "glyph U+{:04X} has a broken point",
                                static_cast<unsigned>(glyph.code));
                contour.push_back(point);
            }
        }
        font.glyphs.push_back(std::move(glyph));
    }

    // Kerning pairs.
    if (has(4)) {
        const std::uint32_t pairs = loadScalar<std::uint32_t>(d, p);
        p += 4;
        if (!has(std::size_t{pairs} * 8))
            return damaged();
        for (std::uint32_t i = 0; i < pairs; ++i, p += 8)
            font.kerning.push_back({loadScalar<std::uint16_t>(d, p), loadScalar<std::uint16_t>(d, p + 2),
                                    loadScalar<float>(d, p + 4)});
    }
    if (p != d.size())
        font.notes.push_back(std::format("{} byte(s) after the kerning table not read", d.size() - p));

    // The baseline is the bottom of the capitals; the line is as high as the tallest shapes reach.
    float low = 0, high = 0;
    bool any = false;
    for (const Font::Glyph& g : font.glyphs)
        for (const auto& contour : g.contours)
            for (const Font::Point& pt : contour) {
                low = any ? std::min(low, pt.y) : pt.y;
                high = any ? std::max(high, pt.y) : pt.y;
                any = true;
            }
    font.lineHeight = any ? std::max(high, 0.0f) - std::min(low, 0.0f) : 0.0f;
    for (const char32_t capital : {U'H', U'E', U'I', U'0', U'1'})
        if (const Font::Glyph* g = font.find(capital); g && !g->contours.empty()) {
            float bottom = g->contours.front().front().y;
            for (const auto& contour : g->contours)
                for (const Font::Point& pt : contour)
                    bottom = std::min(bottom, pt.y);
            font.baseline = bottom;
            break;
        }
    return font;
}

} // namespace viewer::games::blur
