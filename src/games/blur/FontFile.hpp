#pragma once

#include "viewer/core/Font.hpp"

namespace viewer::games::blur {

/// Blur's ".pf" fonts (ui/data/pf): vector outlines exported from TrueType
/// fonts, with spacing and kerning. See docs/formats/blur.md.
class PfFontDecoder final : public FontDecoder {
public:
    std::string_view name() const noexcept override { return "Blur font"; }
    bool canDecode(std::string_view extension, ByteSpan data) const override;
    Result<Font> decode(ByteSpan data) const override;
};

} // namespace viewer::games::blur
