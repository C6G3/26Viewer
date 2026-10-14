#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// A vector (outline) font: each character's shape as closed contours of
/// straight and curved segments, with its spacing and the kerning between pairs.
/// Units are the font's own; y points up from below the baseline.
struct Font {
    /// A contour point, as fonts store them: a point on the outline, or the
    /// control point(s) of the curve leading to the next point on it.
    struct Point {
        enum class Kind : std::uint8_t {
            On,        ///< On the outline; a straight line leads to it.
            Quadratic, ///< Control point of a quadratic curve to the next point.
            Cubic,     ///< First of two control points of a cubic curve to the point after the next.
        };
        float x = 0;
        float y = 0;
        Kind kind = Kind::On;
    };
    struct Glyph {
        char32_t code = 0; ///< Unicode code point.
        float leftSpace = 0;
        float width = 0;
        float rightSpace = 0;
        std::vector<std::vector<Point>> contours; ///< x from 0 at the glyph's left edge.

        /// How far the pen moves on after drawing this character.
        [[nodiscard]] float advance() const noexcept { return leftSpace + width + rightSpace; }
    };
    struct Kerning {
        char32_t left = 0;
        char32_t right = 0;
        float amount = 0; ///< Added to the advance between `left` and `right`.
    };

    std::string name;
    float baseline = 0;   ///< Height of the baseline in font units.
    float lineHeight = 0; ///< Height of a line (the font's em box).
    std::vector<Glyph> glyphs;
    std::vector<Kerning> kerning;
    std::vector<std::string> notes;

    [[nodiscard]] const Glyph* find(char32_t code) const noexcept;
    [[nodiscard]] float kern(char32_t left, char32_t right) const noexcept;
};

/// Reads a font file.
class FontDecoder {
public:
    virtual ~FontDecoder() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecode(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<Font> decode(ByteSpan data) const = 0;
};

} // namespace viewer
