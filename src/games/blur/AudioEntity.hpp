#pragma once

#include "viewer/core/ContentRegistry.hpp"

#include <string>
#include <vector>

namespace viewer::games::blur {

/// Blur's audio entities (audio/dat/*.dat, magic "JBOA"): the Mercury audio
/// engine's node graphs — inputs from the game (car speed, position…), maths
/// components (curves, rolloffs, arrays), sounds, filters and mixers wired
/// together by links "#component:port".
///
///   0x00 "JBOA", u16 version 11, u16 2
///   0x08 u32 string pool offset   0x0c u32 entity name (pool offset)
///   0x10 u32 component count      0x14 u32 update rate
///   0x18 components: u32 type, u32 name (pool offset), payload (size per type)
///
/// Recovered from Blur.exe: header check at 0x562810, component factory switch
/// at 0x5643f8, each type's load function (vtable slot 1) gives its size.
struct AudioEntity {
    struct Component {
        std::uint32_t type = 0;
        std::string_view typeName; ///< "Rolloff", or empty for an unknown type
        std::string name;          ///< "WheelStonesVolumeRolloff"
        std::size_t offset = 0;    ///< Of the type field.
        std::size_t size = 0;      ///< Whole record.
    };
    std::string name;
    std::uint32_t updateRate = 0;
    std::size_t poolOffset = 0;
    std::vector<Component> components;
};

[[nodiscard]] bool isAudioEntity(ByteSpan data) noexcept;
Result<AudioEntity> parseAudioEntity(ByteSpan data);

/// Readable text for an audio entity: every component with its fields,
/// links resolved to component names.
Result<std::string> decompileAudioEntity(ByteSpan data);

class AudioEntityDecompiler final : public TextDecompiler {
public:
    std::string_view name() const noexcept override { return "Blur audio entity"; }
    bool canDecompile(std::string_view extension, ByteSpan data) const override;
    Result<std::string> decompile(ByteSpan data) const override { return decompileAudioEntity(data); }
    /// Numbers can be edited; links, names and list sizes stay.
    bool canRecompile() const noexcept override { return true; }
    Result<ByteBuffer> recompile(ByteSpan original, std::string_view text, const DecompileContext& context) const override;
    TextSyntax syntax() const noexcept override { return TextSyntax::Shader; } // C-style: // comments, numbers
};

/// Lists the components (with offsets) in the Structure tab.
class AudioEntityStructureParser final : public StructureParser {
public:
    std::string_view name() const noexcept override { return "Blur audio entity"; }
    bool canParse(std::string_view extension, ByteSpan data) const override;
    Result<StructureNode> parse(ByteSpan data) const override;
};

} // namespace viewer::games::blur
