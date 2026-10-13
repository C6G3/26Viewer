#pragma once

#include "viewer/core/Animation.hpp"

namespace viewer::games::blur {

/// .skeleton: the bones of an animated model.
class SkeletonFileDecoder final : public SkeletonDecoder {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Blur skeleton"; }
    [[nodiscard]] bool canDecode(std::string_view extension, ByteSpan data) const override;
    [[nodiscard]] Result<Skeleton> decode(ByteSpan data) const override;
};

/// .cycle: an animation for a skeleton.
class CycleDecoder final : public AnimationDecoder {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Blur animation cycle"; }
    [[nodiscard]] bool canDecode(std::string_view extension, ByteSpan data) const override;
    [[nodiscard]] Result<Animation> decode(ByteSpan data) const override;
};

} // namespace viewer::games::blur
