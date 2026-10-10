// Blur model geometry.
//
// The Scene chunk holds an "ARCH" stream: a serialized object graph in which
// every object/field starts with "RA" + u16 version. We don't decode the whole
// graph, only the records that describe geometry:
//
//   vertex buffer  "RA" 00 00, u32 n, u16 elementOffsets[n], u32 vertexCount, blob
//   index buffer   "RA" 01 00, u32 indexCount, u32 0, blob (u16 indices)
//   draw group     "RA" 00 00, u32 n, then n draws (consecutive records, one per
//                  level of detail, most detailed first; unused slots are empty
//                  draws with minIndex 0x7FFFFFFF):
//   draw           "RA" 00 00, u32 startIndex, u32 indexCount, u32 minIndex, u32 vertexCount
//   streams        right after a group: "RA" 00 00, u32 count (1-4), then
//                  "RA" 00 00, u32 buffer, u32 ?, u32 byteOffset, u32 ? — the first
//                  stream's byteOffset / stride is the group's base vertex
//   blob           "RA" 00 00, u32 byteSize, u32 16, zero padding, data — the data
//                  starts at the first 16-byte boundary (counted from "ARCH") that
//                  is at least 5 bytes past the align field.
//
// Draws tile each index buffer from start to end. The stream's buffer number is
// not an index into the file's buffer order, so the buffer for each number is
// solved from the constraints (every group using that number must fit, and a
// buffer belongs to one number). Vertex buffers holding only a position are
// shadow hulls (veh_Shadow.fx on cars); they are marked as such when there is real
// geometry. Indices are triangle strips stitched with degenerate triangles, or
// plain lists. Element 0 of a vertex is the position: 3 floats (12/16 bytes)
// or 4 halves (8 bytes).

#include "ModelDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>

namespace viewer::games::blur {

namespace {

struct Blob {
    std::size_t start = 0;
    std::size_t size = 0;
};

struct VertexBuffer {
    std::vector<std::uint16_t> offsets;
    std::uint32_t count = 0;
    std::uint32_t stride = 0;
    Blob data;
};

struct Draw {
    std::size_t at = 0; ///< File position of the record (groups are consecutive records).
    std::uint32_t start = 0;
    std::uint32_t count = 0;
    std::uint32_t minIndex = 0;
    std::uint32_t vertices = 0;
};

struct Stream {
    std::uint32_t buffer = 0; ///< Buffer number (see the header comment).
    std::uint32_t byteOffset = 0;
};

/// The draws for one mesh part at each level of detail, and where its vertices are.
struct Group {
    const std::vector<std::uint16_t>* indices = nullptr;
    std::vector<Draw> lods;
    std::size_t slots = 0; ///< LOD slots, including empty ones (the part isn't drawn at those levels).
    std::optional<Stream> stream;
    std::uint32_t highestIndex = 0;
    std::uint64_t vertexRange = 0; ///< max(minIndex + vertexCount)
};

class Scanner {
public:
    Scanner(ByteSpan d, std::size_t arch) : m_d(d), m_arch(arch) {}

    std::optional<Blob> blobAt(std::size_t p) const
    {
        if (p + 12 > m_d.size() || !isRA(p, 0))
            return std::nullopt;
        const auto size = u32(p + 4);
        if (u32(p + 8) != 16)
            return std::nullopt;
        const std::size_t rel = p + 12 + 5 - m_arch;
        const std::size_t start = m_arch + rel + ((16 - rel % 16) % 16);
        if (start + std::size_t{size} > m_d.size())
            return std::nullopt;
        for (std::size_t i = p + 12; i < start; ++i)
            if (m_d[i] != std::byte{0})
                return std::nullopt;
        return Blob{start, size};
    }

    bool isRA(std::size_t p, unsigned version) const
    {
        return p + 4 <= m_d.size() && m_d[p] == std::byte{'R'} && m_d[p + 1] == std::byte{'A'}
               && std::to_integer<unsigned>(m_d[p + 2]) == version && m_d[p + 3] == std::byte{0};
    }
    std::uint32_t u32(std::size_t p) const { return loadScalar<std::uint32_t>(m_d, p); }
    std::uint16_t u16(std::size_t p) const { return loadScalar<std::uint16_t>(m_d, p); }

private:
    ByteSpan m_d;
    std::size_t m_arch;
};

std::size_t find(ByteSpan d, std::string_view what, std::size_t from = 0)
{
    const std::string_view hay(reinterpret_cast<const char*>(d.data()), d.size());
    const auto pos = hay.find(what, from);
    return pos == std::string_view::npos ? d.size() : pos;
}

/// Chain of draws covering [0, indexCount) exactly (dynamic programming from the end).
std::optional<std::vector<Draw>> tile(std::size_t indexCount, const std::vector<Draw>& candidates)
{
    std::map<std::uint32_t, std::vector<const Draw*>> byStart;
    for (const Draw& c : candidates)
        if (c.count >= 3 && c.vertices > 0 && std::size_t{c.start} + c.count <= indexCount)
            byStart[c.start].push_back(&c);
    std::map<std::size_t, const Draw*> next; // position -> draw starting there that reaches the end
    std::set<std::size_t> reaches{indexCount};
    for (auto it = byStart.rbegin(); it != byStart.rend(); ++it)
        for (const Draw* c : it->second)
            if (reaches.contains(std::size_t{c->start} + c->count)) {
                reaches.insert(c->start);
                next[c->start] = c;
                break;
            }
    if (!reaches.contains(0))
        return std::nullopt;
    std::vector<Draw> chain;
    for (std::size_t pos = 0; pos < indexCount; pos += chain.back().count)
        chain.push_back(*next.at(pos));
    return chain;
}

/// Does `group` fit in `vb` when its vertices start at the stream's byte offset?
bool fits(const VertexBuffer& vb, const Group& group)
{
    const std::uint32_t offset = group.stream ? group.stream->byteOffset : 0;
    if (offset % vb.stride)
        return false;
    const std::uint64_t first = offset / vb.stride;
    return first + group.highestIndex < vb.count && first + group.vertexRange <= vb.count;
}

/// Buffer number -> vertex buffer, solved like a puzzle: candidates must fit
/// every group using the number; a buffer taken by one number is removed from
/// the others; leftovers take the tightest fit.
std::map<std::uint32_t, std::size_t> solveBuffers(const std::vector<VertexBuffer>& vbs,
                                                  const std::vector<Group>& groups)
{
    std::map<std::uint32_t, std::vector<const Group*>> byNumber;
    for (const Group& g : groups)
        if (g.stream)
            byNumber[g.stream->buffer].push_back(&g);
    std::map<std::uint32_t, std::set<std::size_t>> candidates;
    for (const auto& [number, users] : byNumber)
        for (std::size_t i = 0; i < vbs.size(); ++i)
            if (std::ranges::all_of(users, [&](const Group* g) { return fits(vbs[i], *g); }))
                candidates[number].insert(i);

    std::map<std::uint32_t, std::size_t> mapping;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [number, set] : candidates) {
            if (mapping.contains(number) || set.size() != 1)
                continue;
            const std::size_t buffer = *set.begin();
            mapping[number] = buffer;
            for (auto& [other, otherSet] : candidates)
                if (other != number)
                    otherSet.erase(buffer);
            changed = true;
        }
    }
    for (const auto& [number, set] : candidates) {
        if (mapping.contains(number) || set.empty())
            continue;
        std::uint64_t needed = 0;
        for (const Group* g : byNumber[number])
            needed = std::max(needed, g->vertexRange);
        mapping[number] = *std::ranges::min_element(set, {}, [&](std::size_t i) { return vbs[i].count - needed; });
    }
    return mapping;
}

} // namespace

bool ModelDecoder::canDecode(std::string_view extension, ByteSpan) const
{
    return extension == "model" || extension == "particlemesh";
}

Result<Mesh> ModelDecoder::decode(ByteSpan d) const
{
    const std::size_t arch = find(d, "ARCH");
    if (arch == d.size())
        return fail(ErrorCode::InvalidFormat, "no scene data (ARCH) in this model");
    const Scanner s(d, arch);

    // ---- 1. Records ------------------------------------------------------------------
    std::vector<VertexBuffer> vbs;
    std::vector<std::vector<std::uint16_t>> ibs;
    std::vector<Draw> candidates;
    for (std::size_t p = find(d, "RA", arch); p + 4 <= d.size(); p = find(d, "RA", p)) {
        if (s.isRA(p, 0) && p + 8 <= d.size()) {
            const auto n = s.u32(p + 4);
            if (n >= 1 && n <= 16 && p + 8 + 2 * n + 4 <= d.size()) {
                VertexBuffer vb;
                for (std::uint32_t i = 0; i < n; ++i)
                    vb.offsets.push_back(s.u16(p + 8 + 2 * i));
                const std::size_t q = p + 8 + 2 * n;
                vb.count = s.u32(q);
                if (const auto blob = s.blobAt(q + 4); blob && vb.count && blob->size % vb.count == 0
                                                       && vb.offsets.front() == 0
                                                       && std::ranges::is_sorted(vb.offsets)) {
                    vb.stride = static_cast<std::uint32_t>(blob->size / vb.count);
                    if (vb.offsets.back() < vb.stride && vb.stride <= 256) {
                        vb.data = *blob;
                        vbs.push_back(std::move(vb));
                        p = blob->start + blob->size;
                        continue;
                    }
                }
            }
            if (p + 20 <= d.size())
                candidates.push_back({p, s.u32(p + 4), s.u32(p + 8), s.u32(p + 12), s.u32(p + 16)});
        } else if (s.isRA(p, 1) && p + 12 <= d.size()) {
            const auto count = s.u32(p + 4);
            if (const auto blob = s.blobAt(p + 12); blob && count && blob->size == std::size_t{count} * 2) {
                std::vector<std::uint16_t> indices(count);
                for (std::uint32_t i = 0; i < count; ++i)
                    indices[i] = s.u16(blob->start + 2 * i);
                ibs.push_back(std::move(indices));
                p = blob->start + blob->size;
                continue;
            }
        }
        p += 2;
    }
    if (vbs.empty() || ibs.empty())
        return fail(ErrorCode::InvalidFormat, "no geometry found ({} vertex / {} index buffers)", vbs.size(),
                    ibs.size());

    // ---- 2. Draw groups (levels of detail) and their streams ------------------------------
    std::vector<Group> groups;
    std::size_t untiled = 0;
    for (const auto& ib : ibs) {
        auto chain = tile(ib.size(), candidates);
        if (!chain) {
            ++untiled;
            continue;
        }
        std::ranges::sort(*chain, {}, &Draw::at);
        for (std::size_t i = 0; i < chain->size();) {
            Group g;
            g.indices = &ib;
            g.lods.push_back((*chain)[i++]);
            while (i < chain->size() && (*chain)[i].at == g.lods.back().at + 20)
                g.lods.push_back((*chain)[i++]);
            // Unused LOD slots are empty draws (count 0, minIndex 0x7FFFFFFF) that
            // sit between the real draws and the streams.
            std::size_t end = g.lods.back().at + 20;
            g.slots = g.lods.size();
            while (end + 20 <= d.size() && s.isRA(end, 0) && s.u32(end + 8) == 0 && s.u32(end + 12) == 0x7FFFFFFF) {
                end += 20;
                ++g.slots;
            }
            if (end + 28 <= d.size() && s.isRA(end, 0) && s.isRA(end + 8, 0) && s.u32(end + 4) >= 1
                && s.u32(end + 4) <= 4)
                g.stream = Stream{s.u32(end + 12), s.u32(end + 20)};
            for (const Draw& lod : g.lods) {
                const auto slice = std::span(ib).subspan(lod.start, lod.count);
                g.highestIndex = std::max<std::uint32_t>(g.highestIndex, *std::ranges::max_element(slice));
                g.vertexRange = std::max<std::uint64_t>(g.vertexRange, std::uint64_t{lod.minIndex} + lod.vertices);
            }
            groups.push_back(std::move(g));
        }
    }
    const auto mapping = solveBuffers(vbs, groups);

    // ---- 3. Triangles of every group at every level of detail ------------------------------
    Mesh mesh;
    std::map<std::size_t, std::uint32_t> baseVertex; // vb index -> first vertex in mesh.positions
    const auto addVertexBuffer = [&](std::size_t vbIndex) -> std::optional<std::uint32_t> {
        if (const auto it = baseVertex.find(vbIndex); it != baseVertex.end())
            return it->second;
        const VertexBuffer& vb = vbs[vbIndex];
        const std::uint32_t positionBytes = (vb.offsets.size() > 1 ? vb.offsets[1] : vb.stride) - vb.offsets[0];
        if (positionBytes != 12 && positionBytes != 16 && positionBytes != 8)
            return std::nullopt;
        const auto base = static_cast<std::uint32_t>(mesh.vertexCount());
        for (std::uint32_t v = 0; v < vb.count; ++v) {
            const std::size_t at = vb.data.start + std::size_t{v} * vb.stride;
            for (int axis = 0; axis < 3; ++axis) {
                float value = positionBytes == 8 ? mesh::halfToFloat(s.u16(at + 2 * axis))
                                                 : loadScalar<float>(d, at + 4 * axis);
                if (!std::isfinite(value))
                    value = 0.0f;
                mesh.positions.push_back(value);
            }
        }
        baseVertex[vbIndex] = base;
        return base;
    };

    const bool hasRealGeometry = std::ranges::any_of(vbs, [](const VertexBuffer& vb) { return vb.offsets.size() > 1; });
    std::size_t skipped = untiled;
    std::size_t shadows = 0;
    for (const Group& group : groups) {
        // Which vertex buffer: the solved stream mapping — unless another buffer
        // fits the group exactly and the mapped one doesn't (all-zero streams on
        // shadow hulls are placeholders) — else an exact fit, else the smallest fit.
        const auto exactFit = [&](std::size_t i) {
            const std::uint32_t offset = group.stream ? group.stream->byteOffset : 0;
            return fits(vbs[i], group) && vbs[i].count == offset / vbs[i].stride + group.vertexRange;
        };
        std::optional<std::size_t> buffer;
        if (group.stream)
            if (const auto it = mapping.find(group.stream->buffer); it != mapping.end() && fits(vbs[it->second], group))
                buffer = it->second;
        if (buffer && !exactFit(*buffer))
            for (std::size_t i = 0; i < vbs.size(); ++i)
                if (i != *buffer && exactFit(i)) {
                    buffer = i;
                    break;
                }
        if (!buffer) {
            const std::uint32_t offset = group.stream ? group.stream->byteOffset : 0;
            for (std::size_t i = 0; i < vbs.size(); ++i) {
                if (!fits(vbs[i], group))
                    continue;
                if (vbs[i].count == offset / vbs[i].stride + group.vertexRange) {
                    buffer = i;
                    break;
                }
                if (!buffer || vbs[i].count < vbs[*buffer].count)
                    buffer = i;
            }
        }
        if (!buffer) {
            ++skipped;
            continue;
        }
        const auto bufferStart = addVertexBuffer(*buffer);
        if (!bufferStart) {
            ++skipped;
            continue;
        }
        const std::uint32_t base =
            *bufferStart + (group.stream ? group.stream->byteOffset / vbs[*buffer].stride : 0);

        Mesh::Part part;
        part.shadow = hasRealGeometry && vbs[*buffer].offsets.size() == 1;
        part.group = part.shadow ? std::string("Shadow hulls") : std::format("Buffer {}", *buffer + 1);
        for (std::size_t level = 0; level < group.slots; ++level) {
            Mesh::Range range{static_cast<std::uint32_t>(mesh.indices.size()), 0};
            if (level < group.lods.size()) {
                const Draw& draw = group.lods[level];
                const auto slice = std::span(*group.indices).subspan(draw.start, draw.count);
                // Plain triangle list, or a strip stitched with degenerate triangles?
                bool list = slice.size() % 3 == 0;
                for (std::size_t i = 0; list && i + 2 < slice.size(); i += 3)
                    if (slice[i] == slice[i + 1] || slice[i + 1] == slice[i + 2] || slice[i] == slice[i + 2])
                        list = false;
                const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
                    if (a == b || b == c || a == c)
                        return; // degenerate (strip stitching)
                    mesh.indices.insert(mesh.indices.end(), {base + a, base + b, base + c});
                };
                if (list) {
                    for (std::size_t i = 0; i + 2 < slice.size(); i += 3)
                        emit(slice[i], slice[i + 1], slice[i + 2]);
                } else {
                    for (std::size_t i = 0; i + 2 < slice.size(); ++i) // strips alternate winding
                        i % 2 ? emit(slice[i], slice[i + 2], slice[i + 1]) : emit(slice[i], slice[i + 1], slice[i + 2]);
                }
            }
            range.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - range.firstIndex;
            part.lods.push_back(range);
        }
        if (std::ranges::all_of(part.lods, [](const Mesh::Range& r) { return r.indexCount == 0; }))
            continue;
        shadows += part.shadow;
        part.name = std::format("Part {}", mesh.parts.size() + 1);
        mesh.parts.push_back(std::move(part));
    }
    if (mesh.indices.empty())
        return fail(ErrorCode::InvalidFormat, "no drawable triangles found");
    mesh.notes.push_back(std::format("{} vertex buffer(s), {} index buffer(s), {} part(s), {} detail level(s)",
                                     vbs.size(), ibs.size(), mesh.parts.size(), mesh.lodCount()));
    if (shadows)
        mesh.notes.push_back(std::format("{} shadow-hull part(s) (hidden until ticked in the parts list)", shadows));
    if (skipped)
        mesh.notes.push_back(std::format("{} part(s) not understood yet", skipped));
    return mesh;
}

} // namespace viewer::games::blur
