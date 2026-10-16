#pragma once

#include "viewer/core/ContentRegistry.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

/// The table as the game stores it: the name table sorted by name (the game
/// looks names up by binary search), each language's table sorted by id.
/// Strings without a name are only in the language tables.
[[nodiscard]] ByteBuffer writeLocTable(const LocTable& table);

/// What the text tables say about each text id.
struct LocIndex {
    std::unordered_map<std::uint32_t, std::string> english; ///< id -> English text
    std::unordered_map<std::uint32_t, std::string> names;   ///< id -> name ("VEH_MOD_VIPERACR")
    std::unordered_map<std::string, std::uint32_t> ids;     ///< name -> id
    std::unordered_map<std::uint32_t, std::string> files;   ///< id -> the table it's in ("vehicles.loc")
};

/// Everything the text tables in `files` say (other files are skipped).
[[nodiscard]] LocIndex locIndex(const std::vector<DecompileContext::File>& files);

/// LocId -> English text, from every text table in `files` (others are skipped).
[[nodiscard]] std::unordered_map<std::uint32_t, std::string>
englishTexts(const std::vector<DecompileContext::File>& files);

/// The id a new text called `name` gets. The game never computes ids (it
/// looks names up in the tables), so any unused one works; 26Viewer takes the
/// archive name hash of the name (~CRC-32), moved on past 0 and `used` ids, so
/// a name always gets the same id.
[[nodiscard]] std::uint32_t newTextId(std::string_view name, const std::unordered_set<std::uint32_t>& used);

/// Whether `name` can name a text: letters, digits and _ (like the game's).
[[nodiscard]] bool isTextName(std::string_view name);

/// `text` in double quotes, with \" \\ \n \t escapes (for decompiled listings).
[[nodiscard]] std::string quotedText(std::string_view text);

/// Shows a text table: each text's name and its translations. The texts can
/// be edited and written back (any length), and new texts added: a name line
/// without an id, then its texts (languages left out take the first one's).
class LocTableDecompiler final : public TextDecompiler {
public:
    std::string_view name() const noexcept override { return "Blur text table"; }
    bool canDecompile(std::string_view extension, ByteSpan data) const override;
    Result<std::string> decompile(ByteSpan data) const override;
    bool canRecompile() const noexcept override { return true; }
    std::string_view editHint() const noexcept override
    {
        return "Change the texts (the parts in quotes), or add a text: a new name line (letters, digits, _) "
               "with its texts under it; 26Viewer gives it an id. Existing names and ids must stay.";
    }
    Result<ByteBuffer> recompile(ByteSpan original, std::string_view text, const DecompileContext& context) const override;
    TextSyntax syntax() const noexcept override { return TextSyntax::Shader; } // // comments, "texts"
};

} // namespace viewer::games::blur
