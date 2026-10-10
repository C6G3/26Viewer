#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

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
        std::string name;        ///< "Part 12"
        std::string group;       ///< Folder in the parts tree ("Vertex buffer 2").
        std::vector<Range> lods; ///< [0] = most detailed. An empty range: not drawn at that level.
        bool shadow = false;     ///< A shadow hull (used for the game's shadows; hidden by default).
    };

    std::vector<float> positions;       ///< x, y, z per vertex.
    std::vector<std::uint32_t> indices; ///< Three per triangle (every part at every level).
    std::vector<Part> parts;
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

} // namespace mesh
} // namespace viewer
