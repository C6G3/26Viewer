#include "viewer/core/Font.hpp"

#include <algorithm>

namespace viewer {

const Font::Glyph* Font::find(char32_t code) const noexcept
{
    const auto it = std::ranges::find(glyphs, code, &Glyph::code);
    return it == glyphs.end() ? nullptr : &*it;
}

float Font::kern(char32_t left, char32_t right) const noexcept
{
    const auto it = std::ranges::find_if(kerning, [&](const Kerning& k) { return k.left == left && k.right == right; });
    return it == kerning.end() ? 0.0f : it->amount;
}

} // namespace viewer
