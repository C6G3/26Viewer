#pragma once

#include "viewer/core/ContentRegistry.hpp"

namespace viewer::games::blur {

/// Blur's font tables (ui/data/ui_font_*.dat): for each font drawn from an
/// atlas texture (in the matching .bpr), where every character sits in the
/// atlas, its spacing, and the kerning pairs — shown as text.
class UiFontTableDecompiler final : public TextDecompiler {
public:
    std::string_view name() const noexcept override { return "Blur font table"; }
    bool canDecompile(std::string_view extension, ByteSpan data) const override;
    Result<std::string> decompile(ByteSpan data) const override;
    /// Names, sizes, the atlas rectangles, spacing and kerning can be edited.
    bool canRecompile() const noexcept override { return true; }
    Result<ByteBuffer> recompile(ByteSpan original, std::string_view text, const DecompileContext& context) const override;
};

} // namespace viewer::games::blur
