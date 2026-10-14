#pragma once

#include "viewer/core/ContentRegistry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace viewer::games::blur {

/// Bizarre Creations' tagged chunk container, used by .texture, .model,
/// .font, .particlemesh, .level, .levelrender and .levelphysics.
///
///   char name[]   NUL-terminated, padded with NULs to a multiple of 4 (level
///                 render files: a fixed 8-byte field, junk after the NUL)
///   u32  size     whole chunk including this header
///   u16  count    child count (containers)
///   u16  kind     'C' container / 'A' data
///   ...  payload
struct Chunk {
    std::string name;
    std::size_t offset = 0;      ///< Start of the chunk header.
    std::size_t headerSize = 0;
    std::size_t size = 0;        ///< Header + payload.
    std::uint16_t count = 0;
    char kind = '?';
    std::vector<Chunk> children;

    [[nodiscard]] std::size_t payloadOffset() const noexcept { return offset + headerSize; }
    [[nodiscard]] std::size_t payloadSize() const noexcept { return size - headerSize; }
    [[nodiscard]] const Chunk* child(std::string_view childName) const noexcept;
};

/// Parses the chunk header at `offset`; nullopt if it is not a plausible chunk.
[[nodiscard]] std::optional<Chunk> readChunkHeader(ByteSpan data, std::size_t offset);

/// Parses a whole chunk file into a tree. Fails if the root is not a chunk.
[[nodiscard]] Result<Chunk> parseChunkFile(ByteSpan data);

/// Shows chunk files in the Structure tab.
class ChunkStructureParser final : public StructureParser {
public:
    std::string_view name() const noexcept override { return "Bizarre chunk file"; }
    bool canParse(std::string_view extension, ByteSpan data) const override;
    Result<StructureNode> parse(ByteSpan data) const override;
};

/// Decodes Blur .texture files (a chunk file wrapping D3D surface data).
class TextureDecoder final : public ImageDecoder {
public:
    std::string_view name() const noexcept override { return "Blur texture"; }
    bool canDecode(std::string_view extension, ByteSpan data) const override;
    Result<Image> decode(ByteSpan data) const override;
    /// Blur textures are DXT1 / DXT3 / DXT5 / A8R8G8B8 (and a few A8 masks).
    Result<image::Surfaces> surfaces(ByteSpan data) const override;
    bool canRebuild() const noexcept override { return true; }
    std::vector<image::PixelFormat> importFormats(ByteSpan data) const override;
    Result<ByteBuffer> rebuild(ByteSpan original, const image::Surfaces& surfaces) const override;
};

} // namespace viewer::games::blur
