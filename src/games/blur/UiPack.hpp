#pragma once

#include "viewer/core/ContentRegistry.hpp"

namespace viewer::games::blur {

/// Blur's UI resource packs (".bpr", "2RZB"): the menus' and HUD's textures
/// (logos, icons, rank badges, the font atlas). Listed, read and replaced in
/// place like a model's textures. See docs/formats/blur.md.
class UiPackTextures final : public TextureDictionaryFormat {
public:
    std::string_view name() const noexcept override { return "Blur UI pack"; }
    bool canRead(std::string_view extension, ByteSpan data) const override;
    Result<std::vector<DictionaryTexture>> list(ByteSpan data) const override;
    Result<image::Surfaces> surfaces(ByteSpan data, std::size_t index) const override;
    Result<ByteBuffer> replace(ByteSpan data, std::size_t index, const image::Surfaces& surfaces) const override;
};

} // namespace viewer::games::blur
