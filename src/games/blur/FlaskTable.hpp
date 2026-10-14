#pragma once

#include "viewer/core/ContentRegistry.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace viewer::games::blur {

/// Blur's data tables ("Flask" packs, magic "KSLF"): gamedata/xt/*.bin (car
/// handling in racingphysics.bin, vehicles, events, pickups, UI…) and
/// levels/*/actorpack.xt. Each pack describes its own types — names, fields
/// and their offsets — so every table reads the same way.
struct FlaskPack {
    struct Type {
        std::string name;
        std::uint16_t fieldCount = 0;
        std::uint16_t baseCount = 0;
        std::uint16_t kind = 0; ///< 0 value (see `primitive`), 1 enum, 2 flags, 3 struct
        std::uint16_t primitive = 0; ///< 1 bool, 2 i8, 4 i32, 5 i64, 6 u8, 7 u16, 8 u32, 9 u64, 10 f32, 11 f64, 12 text, 14 LocId
        std::uint16_t size = 0;      ///< Bytes in the file.
        std::size_t firstField = 0;
        std::size_t firstBase = 0;
    };
    struct Field {
        std::string name;
        std::uint16_t type = 0;
        std::uint16_t offset = 0;
        /// 0 value, 1 enum, 2 flags, 3 struct (inline), 4 pointer to an object
        /// of the same record, 5 another record, 6 an enum's item.
        std::uint8_t kind = 0;
        bool array = false; ///< u16 layout, u16 first element, u32 count
    };
    struct Base {
        std::uint16_t type = 0;
        std::uint16_t offset = 0;
    };
    struct Record {
        std::uint32_t id = 0;
        std::uint16_t type = 0;
        std::uint16_t block = 0xFFFF; ///< 0xFFFF: defined in another table.
    };
    /// A run of objects of one type inside a block.
    struct Layout {
        std::uint16_t type = 0;
        std::uint16_t count = 0;
        std::uint16_t kind = 0; ///< 0 the objects themselves, 1 pointers, 2 record references
        std::size_t start = 0;  ///< Byte offset in Block::objects.
        [[nodiscard]] std::size_t elementSize(const FlaskPack& pack) const
        {
            return kind == 0 ? pack.types[type].size : 4;
        }
    };
    /// One record's objects (its own object first) and its texts.
    struct Block {
        std::uint32_t record = 0;
        std::vector<Layout> layouts;
        ByteSpan objects;
        std::string strings; ///< The texts, unscrambled.
        ByteSpan rawText;    ///< The texts as stored.
    };

    std::uint32_t version = 0;
    std::array<std::size_t, 8> sectionOffset{}; ///< Header: where each section starts…
    std::array<std::size_t, 8> sectionCount{};  ///< …and its count.
    std::vector<Type> types;
    std::vector<Field> fields;
    std::vector<Base> bases;
    std::vector<Record> records;
    std::vector<Block> blocks;
};

/// Reads (and checks) a pack. `data` must outlive the result.
[[nodiscard]] Result<FlaskPack> readFlask(ByteSpan data);

/// Undoes the scrambling of a version 2 pack's names and texts.
[[nodiscard]] std::string unscrambleFlaskText(ByteSpan data);

/// Shows a data table as text: every record with its fields and values.
/// Text ids (LocId) are shown with their English text when the game's text
/// tables (gamedata/xt/loc) are at hand. Edited values can be written back:
/// numbers, true/false, enum items, flags, texts and references to records
/// the table lists. Lists can get more or fewer items (new ones are copies
/// of the list's last item, given the edited values); which fields exist stays.
class FlaskDecompiler final : public TextDecompiler {
public:
    std::string_view name() const noexcept override { return "Blur data table"; }
    bool canDecompile(std::string_view extension, ByteSpan data) const override;
    Result<std::string> decompile(ByteSpan data) const override;
    Result<std::string> decompileWith(ByteSpan data, const DecompileContext& context) const override;
    bool canRecompile() const noexcept override { return true; }
    Result<ByteBuffer> recompile(ByteSpan original, std::string_view text, const DecompileContext& context) const override;
    std::string_view editHint() const noexcept override
    {
        return "Change any value. Lists can get longer or shorter: to add an item, copy one and paste it "
               "into the list; to remove one, delete it. Field names and brackets must stay, and whole "
               "records can't be added or removed.";
    }
    TextSyntax syntax() const noexcept override { return TextSyntax::Shader; }
};

} // namespace viewer::games::blur
