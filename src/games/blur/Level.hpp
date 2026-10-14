#pragma once

#include "viewer/core/ContentRegistry.hpp"

namespace viewer::games::blur {

/// A whole track: the render data of levels/<name>/Level.levelrender (the
/// scene, and where everything sits in the stream file) and
/// Level.levelstream (the scene nodes' draw calls, vertex/index buffers and
/// textures). Every mesh is placed with its instance's world matrix.
/// See docs/formats/blur.md ("Levels").
[[nodiscard]] Result<Mesh> decodeLevel(ByteSpan render, ByteSpan stream);

/// Shows a level when either of its two files is opened (it reads the other).
class LevelDecoder final : public MeshDecoder {
public:
    std::string_view name() const noexcept override { return "Blur level"; }
    bool canDecode(std::string_view extension, ByteSpan data) const override;
    Result<Mesh> decode(ByteSpan data) const override;
    Result<Mesh> decodeWith(ByteSpan data, const DecompileContext& context) const override;
};

} // namespace viewer::games::blur
