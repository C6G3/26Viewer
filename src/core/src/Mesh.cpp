#include "viewer/core/Mesh.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace viewer {

std::size_t Mesh::lodCount() const noexcept
{
    std::size_t count = 0;
    for (const Part& part : parts)
        count = std::max(count, part.lods.size());
    return count;
}

std::size_t Mesh::triangleCount(std::size_t lod) const noexcept
{
    std::size_t triangles = 0;
    for (const Part& part : parts)
        if (!part.shadow && lod < part.lods.size())
            triangles += part.lods[lod].indexCount / 3;
    return triangles;
}

} // namespace viewer

namespace viewer::mesh {

float halfToFloat(std::uint16_t h) noexcept
{
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    if (exponent == 0) { // zero / subnormal
        const float value = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -value : value;
    }
    if (exponent == 31) // inf / NaN
        return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

std::string toObj(const Mesh& mesh)
{
    std::string out;
    // OBJ is right-handed: mirror x of left-handed (Direct3D) models and swap the winding.
    const float sx = mesh.leftHanded ? -1.0f : 1.0f;
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v)
        out += std::format("v {} {} {}\n", sx * mesh.positions[3 * v], mesh.positions[3 * v + 1], mesh.positions[3 * v + 2]);
    for (const Mesh::Part& part : mesh.parts) {
        if (part.shadow || part.lods.empty())
            continue;
        out += std::format("g {}_{}\n", part.name, part.material.empty() ? "" : part.material.substr(part.material.rfind(' ') + 1));
        const Mesh::Range& range = part.lods.front();
        for (std::uint32_t i = range.firstIndex; i + 2 < range.firstIndex + range.indexCount; i += 3) {
            const std::uint32_t b = mesh.leftHanded ? i + 2 : i + 1, c = mesh.leftHanded ? i + 1 : i + 2;
            out += std::format("f {} {} {}\n", mesh.indices[i] + 1, mesh.indices[b] + 1, mesh.indices[c] + 1);
        }
    }
    return out;
}

} // namespace viewer::mesh
