#pragma once

#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/Mesh.hpp"

#include <functional>
#include <string>
#include <vector>

namespace viewer::games::blur {

/// One collision shape from a Blur physics stream, turned into triangles in
/// the space of its file (model or level).
struct CollisionShape {
    std::string kind;          ///< "Box", "Convex hull", "Cylinder", "Triangle mesh"…
    std::uint32_t owner = 0;   ///< Handle of the object it belongs to (car parts: 0xC00000nn, the element's render tag).
    std::size_t object = 0;    ///< Index of that object in the stream.
    std::vector<float> positions;           ///< x, y, z per vertex.
    std::vector<std::uint32_t> indices;     ///< Three per triangle.
    std::vector<std::uint16_t> surfaces;    ///< Surface material per triangle (triangle meshes), else empty.
    std::uint16_t material = 0;             ///< Surface material of the whole shape (boxes, hulls…).
};

/// What a physics stream holds, ready to draw.
struct CollisionSet {
    std::vector<CollisionShape> shapes;
    std::size_t objects = 0; ///< Objects read (bodies, shapes, placed copies).
};

/// Reads a physics stream: a model's "Coll" chunk or a level's "Main" /
/// "Route" chunk (see docs/formats/blur.md, "Collision").
[[nodiscard]] Result<CollisionSet> readPhysicsStream(ByteSpan stream);

/// Adds `shapes` to `mesh` as collision parts (one per shape; triangle meshes
/// one per surface material), coloured by material and drawn at every detail
/// level the mesh has. `nameOf` names a shape's part; `nodeOf` gives its node
/// (-1: none).
void appendCollisionParts(Mesh& mesh, const std::vector<CollisionShape>& shapes, const std::string& group,
                          const std::function<std::string(const CollisionShape&)>& nameOf,
                          const std::function<int(const CollisionShape&)>& nodeOf);

/// The game's name for a surface material ("Tarmac", "MetalArmco"…), from
/// its own table (37 entries; the last 3 unnamed).
[[nodiscard]] std::string surfaceName(std::uint16_t material);
/// The colour a surface material is drawn in (0xRRGGBB): tarmac dark grey,
/// grass green, sand beige, water blue…
[[nodiscard]] std::uint32_t surfaceColor(std::uint16_t material) noexcept;

/// Blur ".levelphysics": the level's collision (track, walls and the
/// barriers of each route layout) shown as a mesh.
class LevelPhysicsDecoder final : public MeshDecoder {
public:
    std::string_view name() const noexcept override { return "Blur level collision"; }
    bool canDecode(std::string_view extension, ByteSpan data) const override;
    Result<Mesh> decode(ByteSpan data) const override;
};

} // namespace viewer::games::blur
