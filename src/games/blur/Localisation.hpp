#pragma once

#include "viewer/core/ContentRegistry.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer::games::blur {

/// One of the game's texts, in every language of its table.
struct LocString {
    std::uint32_t id = 0;            ///< What data tables store (a "LocId").
    std::string key;                 ///< The developers' name for it: "VEH_FULL_ASTRA_LEGEND".
    std::vector<std::string> texts;  ///< Per language (UTF-8), in LocTable::languages order.
};

/// A text table (gamedata/xt/loc/*.loc).
struct LocTable {
    std::vector<std::string> languages; ///< "en", "fr", "de", "it", "es"
    std::vector<LocString> strings;
};

[[nodiscard]] Result<LocTable> readLocTable(ByteSpan data);

/// LocId -> English text, from every text table in `files` (others are skipped).
[[nodiscard]] std::unordered_map<std::uint32_t, std::string>
englishTexts(const std::vector<DecompileContext::File>& files);

/// `text` in double quotes, with \" \\ \n \t escapes (for decompiled listings).
[[nodiscard]] std::string quotedText(std::string_view text);

/// Shows a text table: each text's name and its translations. The texts can
/// be edited and written back (any length).
class LocTableDecompiler final : public TextDecompiler {
public:
    std::string_view name() const noexcept override { return "Blur text table"; }
    bool canDecompile(std::string_view extension, ByteSpan data) const override;
    Result<std::string> decompile(ByteSpan data) const override;
    bool canRecompile() const noexcept override { return true; }
    std::string_view editHint() const noexcept override
    {
        return "Change the texts only (the parts in quotes). The names and language codes must stay the same.";
    }
    Result<ByteBuffer> recompile(ByteSpan original, std::string_view text, const DecompileContext& context) const override;
    TextSyntax syntax() const noexcept override { return TextSyntax::Shader; } // // comments, "texts"
};

} // namespace viewer::games::blur
