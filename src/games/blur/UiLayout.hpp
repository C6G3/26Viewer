#pragma once

#include "viewer/core/Drawing.hpp"

namespace viewer::games::blur {

/// Blur's vector UI layouts and icon sheets (".ipf", ui/data/ipf): groups of
/// shapes, each filled and/or outlined, stored already split into triangles.
/// See docs/formats/blur.md.
class IpfDecoder final : public DrawingDecoder {
public:
    std::string_view name() const noexcept override { return "Blur UI layout"; }
    bool canDecode(std::string_view extension, ByteSpan data) const override;
    Result<Drawing> decode(ByteSpan data) const override;
};

} // namespace viewer::games::blur
