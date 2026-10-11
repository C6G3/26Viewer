#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"
#include "viewer/core/Image.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// Geometry ready for display: positions plus triangle lists, split into
/// parts that can be shown/hidden, each with its levels of detail (LODs).
struct Mesh {
    /// A run of `indexCount` indices (triangles) in `indices`.
    struct Range {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };
    /// One piece of the model (one draw call / material in the game).
    struct Part {
        std::string name;        ///< "e_door_left", or "Part 12" when the file has no names
        std::string group;       ///< Folder in the parts tree when there are no nodes ("Buffer 2").
        std::string material;    ///< "Material 10" (several parts of one node differ by material)
        std::string shader;      ///< Effect the game draws it with ("veh_Paint_a_b_c.fx"), if known.
        int node = -1;           ///< Index into Mesh::nodes, or -1.
        std::vector<Range> lods; ///< [0] = most detailed. An empty range: not drawn at that level.
        bool shadow = false;     ///< A shadow hull (used for the game's shadows; hidden by default).
    };
    /// A named node of the model's hierarchy ("e_exterior" > "e_door_left" > "e_mirror_left").
    struct Node {
        std::string name;
        int parent = -1; ///< Index into nodes, or -1 for a root.
    };

    /// A texture stored in the model file itself.
    struct Texture {
        std::string name; ///< "audis3_1"
        Image image;      ///< Most detailed level.
    };

    std::vector<float> positions;       ///< x, y, z per vertex.
    std::vector<float> normals;         ///< x, y, z per vertex (unit length), or empty.
    std::vector<float> uvs;             ///< u, v per vertex (first UV set; v down, as in Direct3D), or empty.
    std::vector<std::uint32_t> indices; ///< Three per triangle (every part at every level).
    std::vector<Part> parts;
    std::vector<Node> nodes; ///< Empty when the format has no hierarchy.
    std::vector<Texture> textures; ///< Textures embedded in the file (cars carry all of theirs).
    /// True when positions are left-handed (Direct3D: x right, y up, z forward);
    /// right-handed viewers mirror x so the model isn't shown back to front.
    bool leftHanded = false;
    std::vector<std::string> notes; ///< Shown in Properties (buffer counts, skipped data...).

    [[nodiscard]] std::size_t vertexCount() const noexcept { return positions.size() / 3; }
    /// Number of detail levels (the most any part has).
    [[nodiscard]] std::size_t lodCount() const noexcept;
    /// Triangles drawn at `lod`, shadow hulls not counted.
    [[nodiscard]] std::size_t triangleCount(std::size_t lod = 0) const noexcept;
};

/// Converts a file's bytes into a Mesh (game model formats).
class MeshDecoder {
public:
    virtual ~MeshDecoder() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecode(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<Mesh> decode(ByteSpan data) const = 0;
};

namespace mesh {

/// IEEE 754 half precision → float.
[[nodiscard]] float halfToFloat(std::uint16_t h) noexcept;

/// Wavefront .obj text for the most detailed level (shadow hulls left out),
/// one group per part ("name_material"), with texture coordinates and normals
/// when the mesh has them. Left-handed models are mirrored so they open the
/// right way round in Blender and other right-handed tools.
[[nodiscard]] std::string toObj(const Mesh& mesh);

} // namespace mesh
} // namespace viewer
