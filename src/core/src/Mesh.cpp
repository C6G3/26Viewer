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
        if (!isHelper(part) && lod < part.lods.size())
            triangles += part.lods[lod].indexCount / 3;
    return triangles;
}

bool Mesh::isHelper(const Part& part) const noexcept
{
    if (part.shadow)
        return true;
    return part.collision
           && std::ranges::any_of(parts, [](const Part& other) { return !other.shadow && !other.collision; });
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

Result<Image> textureImage(const Mesh::Texture& texture)
{
    if (texture.image.width != 0 || texture.stored.data.empty())
        return texture.image;
    const image::Surfaces& s = texture.stored;
    const std::size_t top = image::surfaceSize(s.format, s.width, s.height);
    if (s.data.size() < top)
        return fail(ErrorCode::InvalidFormat, "texture {} is shorter than its largest level", texture.name);
    return image::decodeSurface(ByteSpan(s.data).first(top), s.format, s.width, s.height);
}

std::filesystem::path objPath(const std::filesystem::path& source)
{
    std::filesystem::path out = source;
    if (out.extension() == ".model")
        out.replace_extension(".obj");
    else
        out += ".obj";
    return out;
}

std::string toObj(const Mesh& mesh)
{
    std::string out;
    // OBJ is right-handed: mirror x of left-handed (Direct3D) models and swap the winding.
    const float sx = mesh.leftHanded ? -1.0f : 1.0f;
    const bool uvs = mesh.uvs.size() == 2 * mesh.vertexCount();
    const bool normals = mesh.normals.size() == mesh.positions.size();
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v)
        out += std::format("v {} {} {}\n", sx * mesh.positions[3 * v], mesh.positions[3 * v + 1], mesh.positions[3 * v + 2]);
    if (uvs) // OBJ's v points up, Direct3D's down
        for (std::size_t v = 0; v < mesh.vertexCount(); ++v)
            out += std::format("vt {} {}\n", mesh.uvs[2 * v], 1.0f - mesh.uvs[2 * v + 1]);
    if (normals)
        for (std::size_t v = 0; v < mesh.vertexCount(); ++v)
            out += std::format("vn {} {} {}\n", sx * mesh.normals[3 * v], mesh.normals[3 * v + 1], mesh.normals[3 * v + 2]);
    auto corner = [&](std::uint32_t index) {
        const std::uint32_t n = index + 1;
        if (uvs && normals)
            return std::format("{}/{}/{}", n, n, n);
        if (uvs)
            return std::format("{}/{}", n, n);
        if (normals)
            return std::format("{}//{}", n, n);
        return std::to_string(n);
    };
    for (const Mesh::Part& part : mesh.parts) {
        if (mesh.isHelper(part) || part.lods.empty())
            continue;
        // Group names end at a space in OBJ, so spaces become underscores.
        std::string group = std::format("{}_{}", part.name,
                                        part.material.empty() ? "" : part.material.substr(part.material.rfind(' ') + 1));
        std::ranges::replace(group, ' ', '_');
        out += std::format("g {}\n", group);
        const Mesh::Range& range = part.lods.front();
        for (std::uint32_t i = range.firstIndex; i + 2 < range.firstIndex + range.indexCount; i += 3) {
            const std::uint32_t b = mesh.leftHanded ? i + 2 : i + 1, c = mesh.leftHanded ? i + 1 : i + 2;
            out += std::format("f {} {} {}\n", corner(mesh.indices[i]), corner(mesh.indices[b]), corner(mesh.indices[c]));
        }
    }
    return out;
}

} // namespace viewer::mesh
