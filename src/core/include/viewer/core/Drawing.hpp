#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// A 2D vector drawing (a game's UI layout or icon sheet), already broken
/// into coloured triangles. Coordinates run from (0, 0) at the top left to
/// (width, height).
struct Drawing {
    struct Shape {
        std::string name;                          ///< "layer_1 / num01_a / <path>"
        std::array<float, 4> color{1, 1, 1, 1};    ///< RGBA, 0..1.
        bool outline = false;                      ///< A stroke rather than a fill.
        std::vector<std::array<float, 2>> triangles; ///< Three corners per triangle.
    };
    /// A caption (an icon's name under it on a sheet).
    struct Label {
        std::string text;
        float x = 0; ///< Top left.
        float y = 0;
        float width = 0;
    };
    float width = 1;
    float height = 1;
    std::vector<Shape> shapes;
    std::vector<Label> labels;
    std::vector<std::string> notes;
};

/// Reads a vector drawing file.
class DrawingDecoder {
public:
    virtual ~DrawingDecoder() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecode(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<Drawing> decode(ByteSpan data) const = 0;
};

} // namespace viewer
