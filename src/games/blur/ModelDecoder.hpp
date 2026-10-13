#pragma once

#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/Mesh.hpp"

namespace viewer::games::blur {

/// Geometry of Blur ".model" files (chunk tree CPModel > Render > Scene,
/// whose body is an "ARCH" serialized scene). See docs/formats/blur.md.
class ModelDecoder final : public MeshDecoder {
public:
    std::string_view name() const noexcept override { return "Blur model"; }
    bool canDecode(std::string_view extension, ByteSpan data) const override;
    Result<Mesh> decode(ByteSpan data) const override;
};

/// The textures a Blur model carries (cars hold all of theirs): listed,
/// read and replaced in place — a replacement keeps the texture's size,
/// pixel format and mip levels, so nothing else in the file moves.
class ModelTextureDictionary final : public TextureDictionaryFormat {
public:
    std::string_view name() const noexcept override { return "Blur model textures"; }
    bool canRead(std::string_view extension, ByteSpan data) const override;
    Result<std::vector<DictionaryTexture>> list(ByteSpan data) const override;
    Result<image::Surfaces> surfaces(ByteSpan data, std::size_t index) const override;
    Result<ByteBuffer> replace(ByteSpan data, std::size_t index, const image::Surfaces& surfaces) const override;
};

} // namespace viewer::games::blur
