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
//
// Each vertex buffer is preceded by its element list: one record per element,
// "RA" 00 00, u32 type, u32 0, u32 usage, u32 usage index. Usages are Direct3D's
// (0 position, 3 normal, 5 texture coordinates); the type codes are the
// engine's own: 0x06 float3, 0x07 float4, 0x08 half2, 0x09 half4, 0x0B four
// bytes mapped to -1..1 (normals), 0x0A four bytes.
//
// Materials: a list of effect names ("veh_Paint_a_b_c.fx"); a draw group's
// material number indexes it. Textures are stored in the file: full ones
// (name, "RA" 01 00 header, hash, size, then the .texture Data layout and the
// pixels) and streamed ones, one record per size (16x16, 32x32...) whose
// pixels follow in a blob — the largest holds the most detailed level.
//
// Collision: the "Coll" chunk after Render (see Physics.cpp). A car part's
// body is tied to its element by the element's render tag.

#include "ModelDecoder.hpp"

#include "ChunkFile.hpp"
#include "Physics.hpp"

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
    int normalAt = -1;       ///< Byte offset of the normal within a vertex (-1: none).
    int uvAt = -1;           ///< Byte offset of the first texture coordinates (-1: none).
    std::uint32_t uvType = 0;
    int slotsAt = -1;        ///< Skinning: four bone slots (bytes), or -1.
    int weightsAt = -1;      ///< Skinning: four weights (bytes, 0..255), or -1.
};

// Vertex element type codes (see the comment at the top).
constexpr std::uint32_t kFloat3 = 0x06, kFloat4 = 0x07, kHalf2 = 0x08, kHalf4 = 0x09, kUByte4 = 0x0A, kUByte4N = 0x0B;

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

/// A node of MdlDat's Element table: name, parent and placement.
struct Element {
    std::string name;
    int parent = -1;
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; ///< Row vectors: p' = p * R + position
    float position[3] = {0, 0, 0};
    std::uint32_t tag = 0; ///< Render tag (0xC00000nn); physics bodies use it to name their element.

    [[nodiscard]] bool isIdentity() const noexcept
    {
        static constexpr float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (int i = 0; i < 9; ++i)
            if (std::abs(rotation[i] - identity[i]) > 1e-5f)
                return false;
        return position[0] == 0 && position[1] == 0 && position[2] == 0;
    }
};

/// MdlDat { StrTab, Element }: the model's named hierarchy (empty if absent).
///   StrTab:  u32 count, u32 offsets[count], u32 ?, then NUL-terminated strings
///   Element: 104-byte records — u32 0, f32 rotation[9], f32 position[3],
///            f32 bounds[6], u32 name (string index), u32 index, i32 parent,
///            i32 first child, i32 next sibling, u32 ?, u32 render tag
std::vector<Element> readElements(ByteSpan d)
{
    std::vector<Element> elements;
    auto root = parseChunkFile(d);
    if (!root)
        return elements;
    const Chunk* mdl = root->child("MdlDat");
    const Chunk* strtab = mdl ? mdl->child("StrTab") : nullptr;
    const Chunk* table = mdl ? mdl->child("Element") : nullptr;
    if (!strtab || !table || table->payloadSize() % 104)
        return elements;

    const ByteSpan strings = d.subspan(strtab->payloadOffset(), strtab->payloadSize());
    if (strings.size() < 4)
        return elements;
    const auto count = loadScalar<std::uint32_t>(strings, 0);
    const std::size_t pool = 8 + 4 * std::size_t{count};
    if (pool > strings.size())
        return elements;
    const auto stringAt = [&](std::uint32_t index) -> std::string {
        if (index >= count)
            return {};
        const std::size_t start = pool + loadScalar<std::uint32_t>(strings, 4 + 4 * std::size_t{index});
        std::string out;
        for (std::size_t i = start; i < strings.size() && strings[i] != std::byte{0}; ++i)
            out += static_cast<char>(strings[i]);
        return out;
    };

    const ByteSpan records = d.subspan(table->payloadOffset(), table->payloadSize());
    for (std::size_t r = 0; r < records.size() / 104; ++r) {
        const std::size_t at = r * 104;
        Element e;
        for (int i = 0; i < 9; ++i)
            e.rotation[i] = loadScalar<float>(records, at + 4 + 4 * i);
        for (int i = 0; i < 3; ++i)
            e.position[i] = loadScalar<float>(records, at + 40 + 4 * i);
        e.name = stringAt(loadScalar<std::uint32_t>(records, at + 76));
        const auto parent = loadScalar<std::int32_t>(records, at + 84);
        e.parent = parent >= 0 && static_cast<std::size_t>(parent) < records.size() / 104 ? parent : -1;
        e.tag = loadScalar<std::uint32_t>(records, at + 100);
        if (!std::ranges::all_of(e.rotation, [](float f) { return std::isfinite(f); })
            || !std::ranges::all_of(e.position, [](float f) { return std::isfinite(f); }))
            e = Element{e.name, e.parent, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, e.tag};
        elements.push_back(std::move(e));
    }
    return elements;
}

/// The draws for one mesh part at each level of detail, and where its vertices are.
struct Group {
    const std::vector<std::uint16_t>* indices = nullptr;
    std::vector<Draw> lods;
    std::size_t slots = 0; ///< LOD slots, including empty ones (the part isn't drawn at those levels).
    std::optional<Stream> stream;
    int element = -1;            ///< MdlDat element it belongs to (its name and placement).
    std::uint32_t material = 0;  ///< Material slot.
    int values = -1;             ///< Its material's values (index into the value table), or -1.
    std::vector<int> passValues; ///< Every pass's values (the first is `values`; skinned parts keep their bones in another).
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

/// Moves a part from its element's local space into model space: its vertices
/// are copied (other parts may share them) and transformed, p' = p * R + t.
/// The element list in front of the vertex buffer record at `p` (n elements):
/// where the normal and the first texture coordinates are.
void readVertexElements(const Scanner& s, std::size_t p, std::uint32_t n, VertexBuffer& vb)
{
    if (p < 20 * std::size_t{n})
        return;
    for (std::uint32_t i = 0; i < n; ++i)
        if (const std::size_t q = p - 20 * (n - i); !s.isRA(q, 0) || s.u32(q + 8) != 0)
            return;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t q = p - 20 * (n - i);
        const auto type = s.u32(q + 4), usage = s.u32(q + 12), index = s.u32(q + 16);
        const auto space = (i + 1 < n ? vb.offsets[i + 1] : vb.stride) - vb.offsets[i];
        if (usage == 3 && index == 0 && type == kUByte4N && space >= 3 && vb.normalAt < 0) {
            vb.normalAt = vb.offsets[i];
        } else if (usage == 5 && index == 0 && vb.uvAt < 0
                   && ((type == kHalf2 || type == kHalf4) ? space >= 4 : (type == kFloat3 || type == kFloat4) && space >= 8)) {
            vb.uvAt = vb.offsets[i];
            vb.uvType = type;
        } else if (space >= 4 && type == kUByte4 && ((usage == 5 && index == 3) || usage == 2)) {
            vb.slotsAt = vb.offsets[i]; // skinned characters keep bone slots and weights as texture sets 3 and 4
        } else if (space >= 4 && type == kUByte4N && ((usage == 5 && index == 4) || usage == 1)) {
            vb.weightsAt = vb.offsets[i];
        }
    }
}

/// The material list: "RA" 00 00, u32 count, then per material an empty record
/// and "RA" 00 00, u32 length, the effect's name. Found from its first name.
std::vector<std::string> readShaderNames(const Scanner& s, ByteSpan d, std::size_t arch)
{
    const std::string_view hay(reinterpret_cast<const char*>(d.data()), d.size());
    for (std::size_t fx = hay.find(".fx", arch); fx != std::string_view::npos; fx = hay.find(".fx", fx + 1)) {
        // the record holding this name: "RA" 00 00, u32 length, name
        for (std::uint32_t len = 4; len <= 64 && len <= fx + 3; ++len) {
            const std::size_t name = fx + 3 - len;
            if (name < arch + 16 || !s.isRA(name - 8, 0) || s.u32(name - 4) != len)
                continue;
            const std::size_t first = name - 8 - 4; // the empty record before it
            if (!s.isRA(first, 0) || !s.isRA(first - 8, 0))
                return {};
            const std::uint32_t count = s.u32(first - 4);
            if (count == 0 || count > 256)
                return {};
            std::vector<std::string> names;
            std::size_t p = first;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (p + 12 > d.size() || !s.isRA(p, 0) || !s.isRA(p + 4, 0))
                    break;
                const std::uint32_t n = s.u32(p + 8);
                if (n == 0 || n > 128 || p + 12 + n > d.size())
                    break;
                names.emplace_back(reinterpret_cast<const char*>(d.data() + p + 12), n);
                p += 12 + n;
            }
            return names;
        }
    }
    return {};
}

// ---- Material values -----------------------------------------------------------------------
//
// The render list ("RenderingData::RenderListNode_Common") declares, per pass of
// each material, the effect and its inputs: "RA" 00 00, u32 count, then per
// entry "RA" 00 00 + effect name; "RA" 00 00, u32 n, n x ("RA" 00 00 + name,
// u32 type, u32 count); u32 dataSize, u32 align; "RA" 00 00, u32 n, u16
// offsets[n]; u32 material, u32 0. Types are the game's MaterialElementType:
// 0 S8, 1 U8, 2 S16, 3 U16, 4 S32, 5 U32, 6 F32, 7 TEXTURE_INDEX, 8
// MATERIAL_INDEX, 9 DATA_INDEX (animated data: "RA" 00 00 + two u32), 10
// TEXTURE_PTR. Texture indices number the textures stored in the file in order.
//
// The values follow the draw records: "RA" 00 00, u32 0, u32 count, u32 bytes,
// then count x (u32 render-list entry, that entry's values in declared order).
// A draw group names its material's value set right after its first stream.

struct MaterialInput {
    std::string name;
    std::uint32_t type = 0;
    std::uint32_t count = 0;
};

struct RenderListEntry {
    std::string effect;
    std::vector<MaterialInput> inputs;
    std::uint32_t dataSize = 0;
};

struct MaterialValues {
    std::vector<Mesh::TextureSlot> textures;
    std::vector<Mesh::Constant> constants;
    std::vector<std::uint32_t> palette; ///< "g_mPalette": the bones of a skinned part (DATA_INDEX numbers).
    std::vector<std::string> bones;      ///< The palette's bone names (filled in by readMaterials).
};

/// Where a TEXTURE_INDEX points (Blur.exe builds this table in FUN_006cd820):
/// package -1 is the model's keyed table (`texture`-th entry); any other is a
/// streamed package, numbered in file order.
struct TextureSource {
    std::int32_t package = -1;
    std::uint32_t texture = 0;
};

struct Materials {
    std::vector<MaterialValues> sets;
    std::vector<TextureSource> textures; ///< Per texture index; empty when not found.
};

constexpr std::uint32_t kTextureIndex = 7, kDataIndex = 9, kF32 = 6;

/// A length-prefixed string at `p` (up to 64 printable characters).
std::optional<std::string> stringAt(const Scanner& s, ByteSpan d, std::size_t p)
{
    if (p + 4 > d.size())
        return std::nullopt;
    const std::uint32_t n = s.u32(p);
    if (n > 64 || p + 4 + n > d.size())
        return std::nullopt;
    std::string out(reinterpret_cast<const char*>(d.data() + p + 4), n);
    if (!std::ranges::all_of(out, [](char c) { return c >= 32 && c < 127; }))
        return std::nullopt;
    return out;
}

std::vector<RenderListEntry> readRenderListAt(const Scanner& s, ByteSpan d, std::size_t p, std::size_t& end);

/// The render list; `end` is set to where it stops.
std::vector<RenderListEntry> readRenderList(const Scanner& s, ByteSpan d, std::size_t arch, std::size_t& end)
{
    const std::size_t cls = find(d, "RenderingData::RenderListNode_Common", arch);
    if (cls == d.size())
        return {};
    // The header: "RA" 00 00, u32 count, then the first entry's name record.
    std::size_t p = cls;
    for (; p + 16 <= d.size() && p < cls + 0x400; ++p)
        if (s.isRA(p, 0) && s.isRA(p + 8, 0) && s.u32(p + 4) >= 1 && s.u32(p + 4) <= 4096)
            if (const auto name = stringAt(s, d, p + 12); name && name->size() >= 3)
                break;
    if (p + 16 > d.size() || p >= cls + 0x400)
        return {};
    return readRenderListAt(s, d, p, end);
}

/// A render list from its header at `p` ("RA" 00 00, u32 count, entries).
std::vector<RenderListEntry> readRenderListAt(const Scanner& s, ByteSpan d, std::size_t p, std::size_t& end)
{
    const std::uint32_t count = s.u32(p + 4);
    p += 8;
    std::vector<RenderListEntry> entries;
    for (std::uint32_t e = 0; e < count; ++e) {
        RenderListEntry entry;
        const auto effect = s.isRA(p, 0) ? stringAt(s, d, p + 4) : std::nullopt;
        if (!effect)
            return {};
        entry.effect = *effect;
        p += 8 + effect->size();
        if (!s.isRA(p, 0) || p + 8 > d.size())
            return {};
        const std::uint32_t n = s.u32(p + 4);
        p += 8;
        if (n > 64)
            return {};
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto name = s.isRA(p, 0) ? stringAt(s, d, p + 4) : std::nullopt;
            if (!name || p + 16 + name->size() > d.size())
                return {};
            p += 8 + name->size();
            entry.inputs.push_back({*name, s.u32(p), s.u32(p + 4)});
            if (entry.inputs.back().type > 10 || entry.inputs.back().count > 256)
                return {};
            p += 8;
        }
        if (p + 16 > d.size())
            return {};
        entry.dataSize = s.u32(p);
        p += 8; // dataSize, align
        if (!s.isRA(p, 0))
            return {};
        const std::uint32_t offsets = s.u32(p + 4);
        if (offsets != n)
            return {};
        p += 8 + 2 * std::size_t{offsets} + 8; // offsets, material, 0
        entries.push_back(std::move(entry));
    }
    end = p;
    return entries;
}

/// The animated data a DATA_INDEX names: a second list in the same layout whose
/// entry 0 is "Element" and whose "AnimatedParam" entries name a bone (their
/// one input). Returns the bone name per entry ("" for others).
std::vector<std::string> readDataNames(const Scanner& s, ByteSpan d, std::size_t from)
{
    static constexpr char kElement[] = {7, 0, 0, 0, 'E', 'l', 'e', 'm', 'e', 'n', 't'};
    const std::string_view hay(reinterpret_cast<const char*>(d.data()), d.size());
    for (std::size_t at = hay.find({kElement, sizeof kElement}, from); at != std::string_view::npos;
         at = hay.find({kElement, sizeof kElement}, at + 1)) {
        if (at < 12 || !s.isRA(at - 4, 0) || !s.isRA(at - 12, 0))
            continue;
        std::size_t end = 0;
        const std::vector<RenderListEntry> entries = readRenderListAt(s, d, at - 12, end);
        if (entries.empty())
            continue;
        std::vector<std::string> names;
        for (const RenderListEntry& e : entries)
            names.push_back(e.effect == "AnimatedParam" && !e.inputs.empty() ? e.inputs.front().name : std::string{});
        return names;
    }
    return {};
}

/// Reads count value sets at `p`; empty when they don't parse or their sizes
/// don't add up to `bytes`.
std::vector<MaterialValues> readValueSets(const Scanner& s, ByteSpan d, const std::vector<RenderListEntry>& entries,
                                          std::size_t p, std::uint32_t count, std::uint32_t bytes, std::size_t& end)
{
    std::vector<MaterialValues> sets;
    std::uint64_t total = 0;
    for (std::uint32_t k = 0; k < count; ++k) {
        if (p + 4 > d.size() || s.u32(p) >= entries.size())
            return {};
        const RenderListEntry& entry = entries[s.u32(p)];
        total += entry.dataSize;
        p += 4;
        MaterialValues values;
        for (const MaterialInput& input : entry.inputs) {
            Mesh::Constant constant{input.name, {}};
            for (std::uint32_t i = 0; i < input.count; ++i) {
                if (input.type == kDataIndex) {
                    if (!s.isRA(p, 0) || p + 12 > d.size())
                        return {};
                    if (input.name == "g_mPalette")
                        values.palette.push_back(s.u32(p + 4));
                    p += 12;
                    continue;
                }
                const std::size_t size = input.type <= 1 ? 1 : input.type <= 3 ? 2 : 4;
                if (p + size > d.size())
                    return {};
                if (input.type == kTextureIndex)
                    values.textures.push_back({input.name, static_cast<int>(s.u32(p))});
                else if (input.type == kF32)
                    constant.values.push_back(loadScalar<float>(d, p));
                p += size;
            }
            if (!constant.values.empty())
                values.constants.push_back(std::move(constant));
        }
        sets.push_back(std::move(values));
    }
    if (total != bytes)
        return {};
    end = p;
    return sets;
}

/// A vector of (package, index) pairs at `p` ("RA" 00 00, u32 count, pairs);
/// `next` is set to where it ends.
std::optional<std::vector<TextureSource>> sourcePairs(const Scanner& s, ByteSpan d, std::size_t p, std::size_t& next)
{
    if (p + 8 > d.size() || !s.isRA(p, 0))
        return std::nullopt;
    const std::uint32_t count = s.u32(p + 4);
    if (count > 4096 || p + 8 + 8 * std::size_t{count} > d.size())
        return std::nullopt;
    std::vector<TextureSource> pairs;
    for (std::uint32_t k = 0; k < count; ++k) {
        const auto package = static_cast<std::int32_t>(s.u32(p + 8 + 8 * k));
        const std::uint32_t index = s.u32(p + 12 + 8 * k);
        if (package < -1 || package >= 4096 || (index >= 65536 && index != 0xFFFFFFFF))
            return std::nullopt;
        pairs.push_back({package, index});
    }
    next = p + 8 + 8 * std::size_t{count};
    return pairs;
}

/// The texture table: the scene's three (package, index) vectors follow its
/// material values (Blur.exe's reader FUN_006ea950), the third one being the
/// textures, then another "RA" record.
std::vector<TextureSource> readTextureSources(const Scanner& s, ByteSpan d, std::size_t from)
{
    static constexpr char kRA[] = {'R', 'A', 0, 0};
    const std::string_view hay(reinterpret_cast<const char*>(d.data()), d.size());
    for (std::size_t p = hay.find({kRA, 4}, from); p != std::string_view::npos; p = hay.find({kRA, 4}, p + 1)) {
        std::size_t second = 0, third = 0, after = 0;
        if (!sourcePairs(s, d, p, second) || !sourcePairs(s, d, second, third))
            continue;
        auto textures = sourcePairs(s, d, third, after);
        if (textures && !textures->empty() && after + 4 <= d.size() && s.isRA(after, 0))
            return std::move(*textures);
    }
    return {};
}

/// The material value table (see above), found after the render list, and the
/// texture table after it.
Materials readMaterials(const Scanner& s, ByteSpan d, std::size_t arch)
{
    std::size_t end = 0;
    const std::vector<RenderListEntry> entries = readRenderList(s, d, arch, end);
    if (entries.empty())
        return {};
    static constexpr char kHeader[] = {'R', 'A', 0, 0, 0, 0, 0, 0};
    const std::string_view hay(reinterpret_cast<const char*>(d.data()), d.size());
    for (std::size_t p = hay.find({kHeader, 8}, end); p != std::string_view::npos && p + 16 <= d.size();
         p = hay.find({kHeader, 8}, p + 1)) {
        const std::uint32_t count = s.u32(p + 8), bytes = s.u32(p + 12);
        if (count == 0 || count > 65536 || bytes > d.size())
            continue;
        // A cheap check before parsing: the first set names a render-list entry.
        if (s.u32(p + 16) >= entries.size())
            continue;
        std::size_t after = 0;
        if (auto sets = readValueSets(s, d, entries, p + 16, count, bytes, after); sets.size() == count) {
            if (std::ranges::any_of(sets, [](const MaterialValues& v) { return !v.palette.empty(); })) {
                const std::vector<std::string> names = readDataNames(s, d, end);
                for (MaterialValues& v : sets)
                    for (const std::uint32_t n : v.palette)
                        v.bones.push_back(n < names.size() ? names[n] : std::string{});
            }
            return {std::move(sets), readTextureSources(s, d, after)};
        }
    }
    return {};
}

/// Which stored texture (file order) each texture index means, from the
/// texture table: keyed entries count the keyed textures, streamed packages
/// hold the streamed ones in file order. Empty when the table doesn't fit.
std::vector<int> textureOrder(const std::vector<TextureSource>& sources, const std::vector<bool>& streamed)
{
    std::vector<int> keyed, inPackages;
    for (std::size_t t = 0; t < streamed.size(); ++t)
        (streamed[t] ? inPackages : keyed).push_back(static_cast<int>(t));
    std::vector<std::pair<std::int32_t, std::uint32_t>> packaged;
    for (const TextureSource& src : sources)
        if (src.package >= 0)
            packaged.emplace_back(src.package, src.texture);
    std::ranges::sort(packaged);
    packaged.erase(std::ranges::unique(packaged).begin(), packaged.end());
    if (packaged.size() != inPackages.size())
        return {};
    std::vector<int> order;
    for (const TextureSource& src : sources) {
        if (src.package < 0) {
            if (src.texture >= keyed.size())
                return {};
            order.push_back(keyed[src.texture]);
        } else {
            const auto at = std::ranges::lower_bound(packaged, std::pair{src.package, src.texture});
            order.push_back(inPackages[static_cast<std::size_t>(at - packaged.begin())]);
        }
    }
    return order;
}

/// Consecutive mip levels of a texture stored together, from width x height down.
struct PixelRun {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint32_t width = 0, height = 0;
};

/// A texture stored in the model: its pixels as one run (full textures) or
/// one run per stored size (streamed ones: each larger size holds just its
/// top level, the smallest the rest of the chain) — largest first, together
/// the whole mip chain.
struct StoredTexture {
    std::string name;
    image::PixelFormat format{};
    std::vector<PixelRun> runs;
    std::uint32_t mipCount = 0;
    bool streamed = false; ///< In a streamed group ("RA" 02 before its name), not the keyed table.
};

/// Every texture stored in the file.
std::vector<StoredTexture> findTextures(const Scanner& s, ByteSpan d, std::size_t arch)
{
    struct Candidate {
        std::string name;
        bool first = false; ///< A texture's own record ("RA" 01 header), not a larger streamed size.
        bool streamed = false;
        std::uint32_t width = 0, height = 0;
        image::PixelFormat format{};
        std::size_t pixels = 0, size = 0;
    };
    std::vector<Candidate> found;
    for (std::size_t f = arch + 64; f + 28 <= d.size(); ++f) { // records aren't aligned
        // u32 width, height, depth, mips, format, 0, 3 — find it by the last three
        if (s.u32(f + 20) != 0 || s.u32(f + 24) != 3)
            continue;
        const auto format = image::fromD3DFormat(s.u32(f + 16));
        const auto w = s.u32(f), h = s.u32(f + 4), depth = s.u32(f + 8), mips = s.u32(f + 12);
        // the header in front: "RA" 01 00 and 12 words (full) or 13 (a streamed texture's
        // first size: ..., hash, 1, 28); its larger sizes have just "RA" 00 00, 2, 1, 28
        const std::size_t ra = s.isRA(f - 52, 1) ? f - 52 : s.isRA(f - 56, 1) ? f - 56 : 0;
        const bool nextSize = !ra && s.isRA(f - 16, 0) && s.u32(f - 8) == 1 && s.u32(f - 4) == 28;
        if (!format || w == 0 || h == 0 || w > 4096 || h > 4096 || depth != 1 || mips == 0 || mips > 13
            || (!ra && !nextSize))
            continue;
        Candidate c;
        c.width = w;
        c.height = h;
        c.format = *format;
        if (s.u32(f - 4) == 28) { // streamed: the pixels follow in a blob
            const auto blob = s.blobAt(f + 28);
            if (!blob)
                continue;
            c.pixels = blob->start;
            c.size = blob->size;
        } else {
            c.pixels = f + 28;
            c.size = s.u32(f - 4) - 28;
        }
        if (c.pixels + c.size > d.size() || c.size < image::surfaceSize(c.format, w, h))
            continue;
        c.first = ra != 0;
        // The name in front of the header (length-prefixed; it may be empty or
        // contain spaces: "_ac schnitzer_acs1_coupe").
        for (std::uint32_t len = 0; ra && len <= 64 && len + 4 <= ra; ++len) {
            if (s.u32(ra - len - 4) != len)
                continue;
            std::string name(reinterpret_cast<const char*>(d.data() + ra - len), len);
            if (std::ranges::all_of(name, [](char ch) { return ch >= 32 && ch < 127; }))
                c.name = std::move(name);
            // keyed table: "RA" 00 00, "RA" 00 00, u32 2, name; streamed group: "RA" 02 00, "RA" 00 00, u32 2, name
            c.streamed = ra >= len + 16 && s.isRA(ra - len - 16, 2);
            break;
        }
        found.push_back(std::move(c));
    }
    // One texture per own record, in file order: the materials number them so
    // (two textures may share a name, e.g. several "DefaultColour"). A streamed
    // texture's larger sizes follow its record unnamed; each joins the texture
    // before it when it is the next size up. Then the sizes are chained: each
    // run must start at the level where the one before ended.
    std::vector<StoredTexture> textures;
    for (const Candidate& c : found) {
        const PixelRun run{c.pixels, c.size, c.width, c.height};
        if (c.first) {
            textures.push_back({c.name.empty() ? std::format("texture {}", textures.size() + 1) : c.name, c.format,
                                {run}, 0, c.streamed});
            continue;
        }
        if (textures.empty() || textures.back().format != c.format)
            continue;
        StoredTexture& t = textures.back();
        const auto same = std::ranges::find_if(t.runs, [&](const PixelRun& r) { return r.width == c.width && r.height == c.height; });
        if (same != t.runs.end()) {
            if (c.size > same->size)
                *same = run;
        } else if (const PixelRun& last = t.runs.back(); c.width == 2 * last.width || c.height == 2 * last.height) {
            t.runs.push_back(run);
        }
    }
    for (StoredTexture& t : textures) {
        std::ranges::sort(t.runs, std::greater{}, [](const PixelRun& r) { return std::uint64_t{r.width} * r.height; });
        std::uint32_t w = t.runs.front().width, h = t.runs.front().height;
        std::size_t kept = 0;
        bool complete = false; // reached the 1x1 level
        for (PixelRun& run : t.runs) {
            if (complete || run.width != w || run.height != h)
                break; // a gap in the chain: the rest can't be placed
            std::size_t used = 0;
            while (!complete && run.size - used >= image::surfaceSize(t.format, w, h)) {
                used += image::surfaceSize(t.format, w, h);
                ++t.mipCount;
                complete = w == 1 && h == 1;
                w = std::max(1u, w / 2);
                h = std::max(1u, h / 2);
            }
            run.size = used; // padding after the levels isn't part of the texture
            ++kept;
        }
        t.runs.resize(kept);
    }
    std::erase_if(textures, [](const StoredTexture& t) { return t.runs.empty() || t.mipCount == 0; });
    return textures;
}

/// The textures decoded for display (most detailed level of each).
std::vector<Mesh::Texture> readTextures(const Scanner& s, ByteSpan d, std::size_t arch)
{
    std::vector<Mesh::Texture> out;
    for (const StoredTexture& t : findTextures(s, d, arch)) {
        const PixelRun& top = t.runs.front();
        auto img = image::decodeSurface(d.subspan(top.offset, image::surfaceSize(t.format, top.width, top.height)),
                                        t.format, top.width, top.height);
        if (!img) { // kept (empty) so the materials' texture numbers still line up
            out.push_back({t.name, {}});
            continue;
        }
        img->mipCount = t.mipCount;
        out.push_back({t.name, std::move(*img)});
    }
    return out;
}

/// The stored textures of a whole model file.
std::vector<StoredTexture> modelTextures(ByteSpan d)
{
    const std::size_t arch = find(d, "ARCH");
    if (arch == d.size())
        return {};
    return findTextures(Scanner(d, arch), d, arch);
}

/// The model's collision shapes (its "Coll" chunk) as collision parts, named
/// after the element each body moves with. Returns how many were added.
std::size_t addCollision(ByteSpan d, const std::vector<Element>& elements, Mesh& mesh)
{
    auto root = parseChunkFile(d);
    const Chunk* coll = root ? root->child("Coll") : nullptr;
    if (!coll)
        return 0;
    auto set = readPhysicsStream(d.subspan(coll->payloadOffset(), coll->payloadSize()));
    if (!set) {
        mesh.notes.push_back(std::format("Collision not read: {}", set.error().message));
        return 0;
    }
    std::map<std::uint32_t, int> byTag;
    for (std::size_t i = 0; i < elements.size(); ++i)
        byTag.try_emplace(elements[i].tag, static_cast<int>(i));
    const auto element = [&](const CollisionShape& shape) {
        const auto it = byTag.find(shape.owner);
        return it == byTag.end() ? -1 : it->second;
    };
    const std::size_t before = mesh.parts.size();
    appendCollisionParts(
        mesh, set->shapes, "Collision",
        [&](const CollisionShape& shape) {
            const int e = element(shape);
            return e >= 0 && !elements[static_cast<std::size_t>(e)].name.empty()
                       ? elements[static_cast<std::size_t>(e)].name
                       : std::format("Body {}", shape.object + 1);
        },
        element);
    for (std::size_t p = before; p < mesh.parts.size(); ++p)
        mesh.parts[p].color = 0xE8A33A; // one colour for a model's collision: it's drawn over the model
    return mesh.parts.size() - before;
}

void placePart(Mesh& mesh, Mesh::Part& part, const Element& e)
{
    std::map<std::uint32_t, std::uint32_t> copies;
    for (const Mesh::Range& range : part.lods)
        for (std::uint32_t i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i) {
            const std::uint32_t v = mesh.indices[i];
            auto [it, added] = copies.try_emplace(v, static_cast<std::uint32_t>(mesh.vertexCount()));
            if (added) {
                const float x = mesh.positions[3 * v], y = mesh.positions[3 * v + 1], z = mesh.positions[3 * v + 2];
                const float* r = e.rotation;
                mesh.positions.insert(mesh.positions.end(), {x * r[0] + y * r[3] + z * r[6] + e.position[0],
                                                             x * r[1] + y * r[4] + z * r[7] + e.position[1],
                                                             x * r[2] + y * r[5] + z * r[8] + e.position[2]});
                // normals turn with the part (no translation); uvs are copied
                const float nx = mesh.normals[3 * v], ny = mesh.normals[3 * v + 1], nz = mesh.normals[3 * v + 2];
                float n[3] = {nx * r[0] + ny * r[3] + nz * r[6], nx * r[1] + ny * r[4] + nz * r[7],
                              nx * r[2] + ny * r[5] + nz * r[8]};
                if (const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); len > 0)
                    for (float& c : n)
                        c /= len;
                mesh.normals.insert(mesh.normals.end(), {n[0], n[1], n[2]});
                const float u = mesh.uvs[2 * v], uv = mesh.uvs[2 * v + 1];
                mesh.uvs.insert(mesh.uvs.end(), {u, uv});
                for (int k = 0; k < 4; ++k) {
                    mesh.boneSlots.push_back(mesh.boneSlots[4 * v + k]);
                    mesh.boneWeights.push_back(mesh.boneWeights[4 * v + k]);
                }
            }
            mesh.indices[i] = it->second;
        }
}

} // namespace

// ---- Textures stored in models ------------------------------------------------------------

bool ModelTextureDictionary::canRead(std::string_view extension, ByteSpan) const
{
    return extension == "model" || extension == "particlemesh";
}

Result<std::vector<DictionaryTexture>> ModelTextureDictionary::list(ByteSpan data) const
{
    std::vector<DictionaryTexture> out;
    for (const StoredTexture& t : modelTextures(data))
        out.push_back({t.name, t.format, t.runs.front().width, t.runs.front().height, t.mipCount});
    return out;
}

Result<image::Surfaces> ModelTextureDictionary::surfaces(ByteSpan data, std::size_t index) const
{
    const auto textures = modelTextures(data);
    if (index >= textures.size())
        return fail(ErrorCode::NotFound, "no texture #{} in this model ({} textures)", index + 1, textures.size());
    const StoredTexture& t = textures[index];
    image::Surfaces s{t.format, t.runs.front().width, t.runs.front().height, t.mipCount, 1, {}};
    for (const PixelRun& run : t.runs)
        s.data.insert(s.data.end(), data.begin() + static_cast<std::ptrdiff_t>(run.offset),
                      data.begin() + static_cast<std::ptrdiff_t>(run.offset + run.size));
    return s;
}

Result<ByteBuffer> ModelTextureDictionary::replace(ByteSpan data, std::size_t index, const image::Surfaces& surfaces) const
{
    const auto textures = modelTextures(data);
    if (index >= textures.size())
        return fail(ErrorCode::NotFound, "no texture #{} in this model ({} textures)", index + 1, textures.size());
    const StoredTexture& t = textures[index];
    const std::uint32_t w = t.runs.front().width, h = t.runs.front().height;
    if (surfaces.format != t.format || surfaces.width != w || surfaces.height != h || surfaces.mipCount != t.mipCount
        || surfaces.layers != 1)
        return fail(ErrorCode::Unsupported, "{} must stay {}x{} {} with {} mip level(s)", t.name, w, h,
                    image::pixelFormatName(t.format), t.mipCount);
    std::size_t total = 0;
    for (const PixelRun& run : t.runs)
        total += run.size;
    if (surfaces.data.size() < total)
        return fail(ErrorCode::Unsupported, "{} needs {} bytes of pixels, got {}", t.name, total, surfaces.data.size());
    // Same size, format and levels: the pixels are overwritten where they are,
    // so the rest of the file doesn't move.
    ByteBuffer out(data.begin(), data.end());
    std::size_t from = 0;
    for (const PixelRun& run : t.runs) {
        std::copy_n(surfaces.data.begin() + static_cast<std::ptrdiff_t>(from), run.size,
                    out.begin() + static_cast<std::ptrdiff_t>(run.offset));
        from += run.size;
    }
    return out;
}

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
                        readVertexElements(s, p, n, vb);
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
                && s.u32(end + 4) <= 4) {
                g.stream = Stream{s.u32(end + 12), s.u32(end + 20)};
                // Per pass (u32 at end + 4): a stream record and the pass's material
                // values, "RA" 00 00, u32 value set, u32 0 — 32 bytes in all.
                for (std::uint32_t pass = 0; pass < s.u32(end + 4); ++pass) {
                    const std::size_t at = end + 28 + 32 * std::size_t{pass};
                    if (at + 12 > d.size() || !s.isRA(at, 0) || s.u32(at + 8) != 0)
                        break;
                    g.passValues.push_back(static_cast<int>(s.u32(at + 4)));
                }
                if (!g.passValues.empty())
                    g.values = g.passValues.front();
            }
            // Before the group: "RA" 01 00, u32 material, u32 ?, u32 ?, u32 ?, u32 element
            // (low 16 bits; the high bits are flags).
            if (const std::size_t first = g.lods.front().at; first >= 0x54 && s.isRA(first - 0x54, 1)) {
                g.material = s.u32(first - 0x50);
                g.element = static_cast<int>(s.u32(first - 0x40) & 0xFFFF);
            }
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
    mesh.leftHanded = true; // Direct3D
    const std::vector<Element> elements = readElements(d);
    for (const Element& e : elements) {
        Mesh::Node node{e.name, e.parent};
        std::copy(std::begin(e.rotation), std::end(e.rotation), node.transform.begin());
        std::copy(std::begin(e.position), std::end(e.position), node.transform.begin() + 9);
        mesh.nodes.push_back(std::move(node));
    }
    std::map<std::size_t, std::uint32_t> baseVertex; // vb index -> first vertex in mesh.positions
    bool hasNormals = false, hasUvs = false, hasSkin = false; // these stay as long as positions until the end
    const std::vector<std::string> shaders = readShaderNames(s, d, arch);
    const Materials materials = readMaterials(s, d, arch);
    const std::vector<MaterialValues>& materialValues = materials.sets;
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
            if (vb.normalAt >= 0) {
                for (int axis = 0; axis < 3; ++axis)
                    mesh.normals.push_back(
                        std::to_integer<std::uint8_t>(d[at + static_cast<std::size_t>(vb.normalAt) + axis]) / 127.5f - 1.0f);
                hasNormals = true;
            } else {
                mesh.normals.insert(mesh.normals.end(), {0.0f, 0.0f, 0.0f});
            }
            if (vb.uvAt >= 0) {
                const std::size_t uv = at + static_cast<std::size_t>(vb.uvAt);
                const bool half = vb.uvType == kHalf2 || vb.uvType == kHalf4;
                for (int k = 0; k < 2; ++k) {
                    const float value = half ? mesh::halfToFloat(s.u16(uv + 2 * k)) : loadScalar<float>(d, uv + 4 * k);
                    mesh.uvs.push_back(std::isfinite(value) ? value : 0.0f);
                }
                hasUvs = true;
            } else {
                mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
            }
            if (vb.slotsAt >= 0 && vb.weightsAt >= 0) {
                for (int k = 0; k < 4; ++k) {
                    mesh.boneSlots.push_back(std::to_integer<std::uint8_t>(d[at + static_cast<std::size_t>(vb.slotsAt) + k]));
                    mesh.boneWeights.push_back(
                        std::to_integer<std::uint8_t>(d[at + static_cast<std::size_t>(vb.weightsAt) + k]) / 255.0f);
                }
                hasSkin = true;
            } else {
                mesh.boneSlots.insert(mesh.boneSlots.end(), {0, 0, 0, 0});
                mesh.boneWeights.insert(mesh.boneWeights.end(), {0.0f, 0.0f, 0.0f, 0.0f});
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
        const bool named = group.element >= 0 && static_cast<std::size_t>(group.element) < elements.size()
                           && !elements[static_cast<std::size_t>(group.element)].name.empty();
        part.node = named ? group.element : -1;
        part.name = named ? elements[static_cast<std::size_t>(group.element)].name
                          : std::format("Part {}", mesh.parts.size() + 1);
        part.material = std::format("Material {}", group.material);
        if (group.material < shaders.size())
            part.shader = shaders[group.material];
        if (group.values >= 0 && static_cast<std::size_t>(group.values) < materialValues.size()) {
            part.textures = materialValues[static_cast<std::size_t>(group.values)].textures;
            part.constants = materialValues[static_cast<std::size_t>(group.values)].constants;
        }
        for (const int set : group.passValues)
            if (set >= 0 && static_cast<std::size_t>(set) < materialValues.size() && part.bones.empty())
                part.bones = materialValues[static_cast<std::size_t>(set)].bones;
        if (named && !elements[static_cast<std::size_t>(group.element)].isIdentity())
            placePart(mesh, part, elements[static_cast<std::size_t>(group.element)]);
        mesh.parts.push_back(std::move(part));
    }
    if (mesh.indices.empty())
        return fail(ErrorCode::InvalidFormat, "no drawable triangles found");
    if (!hasNormals)
        mesh.normals.clear();
    if (!hasUvs)
        mesh.uvs.clear();
    if (!hasSkin || std::ranges::none_of(mesh.parts, [](const Mesh::Part& p) { return !p.bones.empty(); })) {
        mesh.boneSlots.clear();
        mesh.boneWeights.clear();
    }
    mesh.textures = readTextures(s, d, arch);
    // Texture indices go through the texture table to the stored textures (one
    // that didn't decode is kept empty, so positions line up). Without a table
    // the stored order is used, unless some textures are streamed.
    std::vector<bool> streamed;
    for (const StoredTexture& t : findTextures(s, d, arch))
        streamed.push_back(t.streamed);
    const std::vector<int> order = textureOrder(materials.textures, streamed);
    const bool unmapped = order.empty() && std::ranges::find(streamed, true) != streamed.end();
    for (Mesh::Part& part : mesh.parts)
        for (Mesh::TextureSlot& slot : part.textures) {
            if (!order.empty())
                slot.texture = slot.texture >= 0 && static_cast<std::size_t>(slot.texture) < order.size()
                                   ? order[static_cast<std::size_t>(slot.texture)]
                                   : -1;
            else if (unmapped)
                slot.texture = -1;
            // a slot past the end is a texture this file doesn't hold
            if (slot.texture < 0 || static_cast<std::size_t>(slot.texture) >= mesh.textures.size())
                slot.texture = -1;
        }
    const auto textured = std::ranges::count_if(mesh.parts, [](const Mesh::Part& p) { return !p.textures.empty(); });
    if (!materialValues.empty())
        mesh.notes.push_back(std::format("{} material value set(s); {} part(s) with textures", materialValues.size(),
                                         textured));
    if (unmapped && textured)
        mesh.notes.push_back("This model's texture table wasn't found, so its streamed textures aren't shown");
    const std::size_t collisionParts = addCollision(d, elements, mesh);
    mesh.notes.push_back(std::format("{} vertex buffer(s), {} index buffer(s), {} part(s), {} detail level(s)",
                                     vbs.size(), ibs.size(), mesh.parts.size(), mesh.lodCount()));
    if (shadows)
        mesh.notes.push_back(std::format("{} shadow-hull part(s) (hidden until ticked in the parts list)", shadows));
    if (skipped)
        mesh.notes.push_back(std::format("{} part(s) not understood yet", skipped));
    if (collisionParts)
        mesh.notes.push_back(std::format("{} collision shape(s) (shown with \"Collision\")", collisionParts));
    if (!mesh.textures.empty()) {
        std::string list;
        for (const Mesh::Texture& t : mesh.textures)
            list += std::format("{}{} ({}x{} {})", list.empty() ? "" : ", ", t.name, t.image.width, t.image.height,
                                t.image.format);
        mesh.notes.push_back(std::format("{} texture(s) in the file: {}", mesh.textures.size(), list));
    }
    return mesh;
}

} // namespace viewer::games::blur
