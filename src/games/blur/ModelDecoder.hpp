#pragma once

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

} // namespace viewer::games::blur
