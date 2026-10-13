#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// Rotation (unit quaternion x, y, z, w), position and scale of a bone
/// relative to its parent.
struct Transform {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> scale{1, 1, 1};
};

/// An affine transform for row vectors: p' = p * R + t, stored as the three
/// rows of R then t (Direct3D's 4x3 layout).
using Matrix43 = std::array<float, 12>;

/// The bones a model is posed with (like an armature in Blender).
struct Skeleton {
    struct Bone {
        std::string name;
        int parent = -1;       ///< Index into bones (parents come first), or -1 for a root.
        Transform rest;        ///< Rest pose, relative to the parent.
        Matrix43 inverseBind{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; ///< Model space → bone space at rest.
    };
    std::vector<Bone> bones;

    /// Index of the bone called `name` (case-insensitive), or -1.
    [[nodiscard]] int find(std::string_view name) const noexcept;
};

/// Keyframes for some bones of a skeleton (like an action in Blender).
struct Animation {
    enum class Kind : std::uint8_t { Rotation, Position, Scale };
    /// One property of one bone: values at key frames (a single key: constant).
    struct Channel {
        std::string bone;
        Kind kind = Kind::Rotation;
        std::vector<float> frames;                ///< Frame of each key, ascending.
        std::vector<std::array<float, 4>> values; ///< Quaternion, or x, y, z (and 0).
    };

    std::uint32_t frameCount = 1; ///< Frames 0 .. frameCount - 1.
    float fps = 30.0f;
    std::vector<std::string> bones; ///< Bones the file was made for.
    std::vector<Channel> channels;
    std::vector<std::string> notes; ///< Shown to the user (parts not decoded...).

    [[nodiscard]] float duration() const noexcept { return fps > 0 ? static_cast<float>(frameCount - 1) / fps : 0.0f; }
};

/// Reads a skeleton file.
class SkeletonDecoder {
public:
    virtual ~SkeletonDecoder() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecode(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<Skeleton> decode(ByteSpan data) const = 0;
};

/// Reads an animation file.
class AnimationDecoder {
public:
    virtual ~AnimationDecoder() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecode(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<Animation> decode(ByteSpan data) const = 0;
};

namespace anim {

/// Rotation matrix of a unit quaternion, for row vectors (p' = p * R).
[[nodiscard]] Matrix43 toMatrix(const Transform& t) noexcept;
/// a then b: p * a * b.
[[nodiscard]] Matrix43 multiply(const Matrix43& a, const Matrix43& b) noexcept;
/// p * m.
[[nodiscard]] std::array<float, 3> apply(const Matrix43& m, const std::array<float, 3>& p) noexcept;
/// The direction d * R (no translation).
[[nodiscard]] std::array<float, 3> rotate(const Matrix43& m, const std::array<float, 3>& d) noexcept;
/// The inverse of an affine transform (identity if it can't be inverted).
[[nodiscard]] Matrix43 inverse(const Matrix43& m) noexcept;
/// Rotation (and position) of a matrix without scale or shear.
[[nodiscard]] Transform decompose(const Matrix43& m) noexcept;
/// A skeleton made of a model's own node hierarchy: one bone per node, posed
/// where the nodes are (`parents[i]`, `transforms[i]` in model space).
[[nodiscard]] Skeleton fromHierarchy(const std::vector<std::string>& names, const std::vector<int>& parents,
                                     const std::vector<Matrix43>& transforms);
[[nodiscard]] std::array<float, 4> slerp(const std::array<float, 4>& a, std::array<float, 4> b, float t) noexcept;

/// Each bone's local transform at `frame` (wrapping around the animation's
/// length): animated properties from `animation`, the rest from the skeleton.
[[nodiscard]] std::vector<Transform> sample(const Skeleton& skeleton, const Animation* animation, float frame);
/// Model-space matrices of the bones for local transforms `locals`.
[[nodiscard]] std::vector<Matrix43> worldMatrices(const Skeleton& skeleton, const std::vector<Transform>& locals);
/// Skinning matrices: inverseBind * world, per bone (identity at rest).
[[nodiscard]] std::vector<Matrix43> skinMatrices(const Skeleton& skeleton, const std::vector<Matrix43>& world);

/// How many of `names` the skeleton has (to pick the skeleton that fits a model).
[[nodiscard]] std::size_t matchCount(const Skeleton& skeleton, const std::vector<std::string>& names) noexcept;
/// Whether every bone the animation moves is in the skeleton.
[[nodiscard]] bool fits(const Skeleton& skeleton, const Animation& animation) noexcept;

} // namespace anim
} // namespace viewer
