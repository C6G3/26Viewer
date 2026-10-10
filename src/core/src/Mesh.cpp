#include "viewer/core/Mesh.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

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

} // namespace viewer::mesh
