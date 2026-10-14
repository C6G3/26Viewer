// Blur levels: Level.levelrender + Level.levelstream (docs/formats/blur.md, "Levels").
//
// Both files hold "ARCH" streams of serialised objects. Every value is "RA" +
// u16 version followed by its fields; a vector is a u32 count and its items.
// The layouts below follow Blur.exe's binary readers (RenderingData::*,
// 0x6D5000-0x6F9000); tools/research/blur_level.py is the reference reader.
//
// Level data is split into numbered blocks, each with ten detail levels (level
// 0 holds the objects; the others only bigger texture mips). The render file's
// Scene ends with the stream's table of contents: per 2 KiB-aligned section of
// the stream, its offset and size and the (block, level) pairs it holds, in
// order. A ResourceIndex {block, index} names the index-th vertex buffer, index
// buffer... of a block.
//
// Scene-node blocks hold a CullNode and one or two RenderListNode_Commons: material types
// and value sets (an "Instance" set holds a world matrix), the geometry (index
// ranges, the index buffer slot, vertex streams) and the draws (a geometry
// drawn with an instance's values). The node's ResourceIndex lists map the
// geometry's index-buffer and vertex-buffer slots to data blocks.

#include "Level.hpp"

#include "ChunkFile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <tuple>
#include <unordered_map>

namespace viewer::games::blur {

namespace {

/// Texture references to this block name the render file's own textures.
constexpr std::uint32_t kSharedBlock = 0xFFFFFFFF;

/// Larger textures are shown at this size (their next smaller copy).
constexpr std::uint32_t kMaxTextureSize = 1024;

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/// Bounds-checked reader of the "RA" serialisation. A failed read sets the
/// error flag and returns zeros; callers check ok() when they're done.
class Reader {
public:
    Reader(ByteSpan d, std::size_t p) : m_d(d), m_p(p) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t pos() const noexcept { return m_p; }
    void fail() noexcept { m_ok = false; }

    bool has(std::size_t n) noexcept
    {
        if (m_ok && m_p <= m_d.size() && m_d.size() - m_p >= n)
            return true;
        m_ok = false;
        return false;
    }
    std::uint16_t ra()
    {
        if (!has(4) || m_d[m_p] != std::byte{'R'} || m_d[m_p + 1] != std::byte{'A'}) {
            m_ok = false;
            return 0;
        }
        const auto v = loadScalar<std::uint16_t>(m_d, m_p + 2);
        m_p += 4;
        return v;
    }
    template <class T>
    T get()
    {
        if (!has(sizeof(T)))
            return T{};
        const T v = loadScalar<T>(m_d, m_p);
        m_p += sizeof(T);
        return v;
    }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    float f32() { return get<float>(); }
    void skip(std::size_t n)
    {
        if (has(n))
            m_p += n;
    }
    std::string str()
    {
        const std::uint32_t n = u32();
        if (n > 4096 || !has(n))
            return {};
        std::string s(reinterpret_cast<const char*>(m_d.data() + m_p), n);
        m_p += n;
        return s;
    }
    /// A vector's count, refused when the rest of the data couldn't hold
    /// that many items of at least `itemBytes` each.
    std::uint32_t count(std::size_t itemBytes)
    {
        const std::uint32_t n = u32();
        if (m_ok && std::uint64_t{n} * itemBytes > m_d.size() - m_p)
            m_ok = false;
        return m_ok ? n : 0;
    }
    /// "RA" 0, u32 index, u32 kind — kept as the pair.
    std::pair<std::uint32_t, std::uint32_t> packed()
    {
        ra();
        const std::uint32_t a = u32();
        return {a, u32()};
    }

private:
    ByteSpan m_d;
    std::size_t m_p;
    bool m_ok = true;
};

using ResIdx = std::pair<std::uint32_t, std::uint32_t>; ///< {block, index}

// ---- scene nodes -------------------------------------------------------------------

void skipNode(Reader& r) // RenderingData::Node: name, SubNodes, Parent, Location
{
    r.ra();
    r.str();
    r.ra();
    r.skip(std::size_t{r.count(8)} * 8);
    r.skip(16);
}

void skipBounds(Reader& r) // AabbMin, AabbMax, DistanceMin, DistanceMax
{
    r.ra();
    r.skip(32);
}

void skipBitVector(Reader& r)
{
    r.ra();
    r.u32();
    r.ra();
    r.skip(r.count(1));
}

void skipCullNode(Reader& r, unsigned version) // Node, Bounds, PvsBits, GroupIndex, PortalBits
{
    skipNode(r);
    skipBounds(r);
    skipBitVector(r);
    if (version >= 1)
        r.u32();
    if (version >= 2)
        skipBitVector(r);
}

struct MaterialElement {
    std::string name;
    std::uint32_t kind = 0;
    std::uint32_t count = 0;
};

struct MaterialType {
    std::string effect;
    std::vector<MaterialElement> elements;
};

/// A material value set: its type and its values, one word each (bytes and
/// shorts widened, texture references as index | kind << 28).
struct ValueSet {
    std::uint32_t type = 0;
    std::vector<std::uint32_t> words;
};

MaterialType readMaterialType(Reader& r)
{
    MaterialType t;
    r.ra();
    t.effect = r.str();
    r.ra();
    const std::uint32_t n = r.count(16);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        r.ra();
        MaterialElement e;
        e.name = r.str();
        e.kind = r.u32();
        e.count = r.u32();
        t.elements.push_back(std::move(e));
    }
    r.skip(8); // DataSize, MaxAlign
    r.ra();
    r.skip(std::size_t{r.count(2)} * 2); // Offsets
    r.skip(8);
    return t;
}

std::vector<ValueSet> readValueSets(Reader& r, const std::vector<MaterialType>& types)
{
    const std::uint32_t n = r.count(4);
    r.u32(); // bytes the values take in memory
    std::vector<ValueSet> sets;
    sets.reserve(n);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        ValueSet set;
        set.type = r.u32();
        if (set.type >= types.size()) {
            r.fail();
            break;
        }
        for (const MaterialElement& e : types[set.type].elements)
            for (std::uint32_t k = 0; k < e.count && r.ok(); ++k) {
                switch (e.kind) {
                case 0:
                case 1: set.words.push_back(r.get<std::uint8_t>()); break;
                case 2:
                case 3: set.words.push_back(r.get<std::uint16_t>()); break;
                case 4:
                case 5:
                case 6:
                case 7:
                case 10: set.words.push_back(r.u32()); break;
                case 8:
                case 9: {
                    const auto [a, b] = r.packed();
                    set.words.push_back((a & 0x0FFFFFFF) | b << 28);
                    break;
                }
                default: r.fail();
                }
            }
        sets.push_back(std::move(set));
    }
    return sets;
}

struct MaterialList {
    std::vector<MaterialType> types;
    std::vector<ValueSet> sets;
};

MaterialList readMaterialList(Reader& r)
{
    MaterialList m;
    r.ra();
    r.ra();
    const std::uint32_t n = r.count(8);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i)
        m.types.push_back(readMaterialType(r));
    m.sets = readValueSets(r, m.types);
    return m;
}

struct Stream {
    std::uint32_t slot = 0;   ///< Vertex-buffer slot (the node's vertex-buffer list).
    std::uint32_t number = 0; ///< Stream number (0: positions...).
    std::uint32_t offset = 0; ///< Byte offset into the buffer.
    ResIdx values{};          ///< The material values it's drawn with.
};

Stream readStream(Reader& r)
{
    r.ra();
    Stream s;
    s.slot = r.u32();
    s.number = r.u32();
    s.offset = r.u32();
    r.u32();
    s.values = r.packed();
    return s;
}

struct IndexRange {
    std::uint32_t start = 0, count = 0, minIndex = 0, vertices = 0;
};

struct Geometry {
    std::uint32_t primitive = 0; ///< 0 triangle list, 1 triangle strip.
    std::uint32_t indexSlot = 0; ///< Index-buffer slot (the node's index-buffer list).
    Mesh::Part::Alpha alpha = Mesh::Part::Alpha::Opaque;
    std::vector<IndexRange> ranges;
    std::vector<Stream> streams;
};

Geometry readGeometry(Reader& r)
{
    Geometry g;
    const unsigned version = r.ra();
    r.skip(8);
    g.primitive = r.u32();
    g.indexSlot = r.u32();
    r.u32();
    r.packed();
    if (version)
        r.packed();
    // Render states: u32 × 3, then four switches (byte 0 alpha blending,
    // byte 3 alpha test: puddles and race banners blend, foliage cuts out),
    // u32 × 2.
    r.ra();
    r.skip(12);
    const auto blend = r.get<std::uint8_t>();
    r.skip(2);
    const auto test = r.get<std::uint8_t>();
    r.skip(8);
    g.alpha = blend ? Mesh::Part::Alpha::Blend : test ? Mesh::Part::Alpha::CutOut : Mesh::Part::Alpha::Opaque;
    r.ra();
    const std::uint32_t nr = r.count(20);
    for (std::uint32_t i = 0; i < nr && r.ok(); ++i) {
        r.ra();
        IndexRange x;
        x.start = r.u32();
        x.count = r.u32();
        x.minIndex = r.u32();
        x.vertices = r.u32();
        g.ranges.push_back(x);
    }
    r.ra();
    const std::uint32_t ns = r.count(28);
    for (std::uint32_t i = 0; i < ns && r.ok(); ++i)
        g.streams.push_back(readStream(r));
    return g;
}

std::vector<Geometry> readGeometryList(Reader& r)
{
    r.ra();
    std::vector<Geometry> out;
    const std::uint32_t n = r.count(40);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i)
        out.push_back(readGeometry(r));
    return out;
}

struct Draw {
    std::uint32_t geometry = 0; ///< Index into the node's geometry.
    ResIdx instance{};          ///< Its instance's material values (world matrix...).
};

Draw readDraw(Reader& r)
{
    Draw d;
    r.ra();
    d.geometry = r.u32();
    d.instance = r.packed();
    r.ra();
    const std::uint32_t n = r.count(28);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i)
        readStream(r);
    r.u32();
    return d;
}

struct SceneNode {
    std::uint32_t block = 0;
    MaterialList materials;           ///< Types (its own value sets are empty in levels).
    std::vector<ValueSet> values;     ///< The node's value sets (of those types): what references of kind 0 name.
    std::vector<MaterialList> extraMaterials;
    std::vector<Geometry> staticGeometry; ///< Drawn as it is, already in world space.
    std::vector<Geometry> geometry;       ///< Drawn by the draws, each instance placed by its world matrix.
    std::vector<Draw> draws;
    std::vector<ResIdx> indexBuffers, vertexBuffers, textures;
};

std::vector<ResIdx> readResList(Reader& r)
{
    r.ra();
    std::vector<ResIdx> out(r.count(8));
    for (auto& x : out) {
        x.first = r.u32();
        x.second = r.u32();
    }
    return out;
}

/// RenderingData::RenderListNode_Common (Blur.exe 0x6EA950).
bool readRenderList(Reader& r, SceneNode& node)
{
    r.ra(); // the pointer's own header
    const unsigned version = r.ra();
    skipCullNode(r, r.ra());
    if (version < 2)
        return false; // older layout: not used by the shipped levels
    node.materials = readMaterialList(r);
    node.staticGeometry = readGeometryList(r); // drawn as it is (the world itself)
    {
        r.ra();
        const std::uint32_t n = r.count(28);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i)
            node.draws.push_back(readDraw(r));
    }
    {
        r.ra(); // instanced draws: a draw plus its instance stream
        const std::uint32_t n = r.count(40);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            r.ra();
            readDraw(r);
            readStream(r);
        }
    }
    node.geometry = readGeometryList(r);
    node.values = readValueSets(r, node.materials.types);
    r.ra();
    r.skip(std::size_t{r.count(28)} * 28); // RA + six words each
    for (int k = 0; k < 3; ++k) {
        r.ra();
        const std::uint32_t n = r.count(36);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i)
            skipBounds(r);
    }
    for (int k = 0; k < 3; ++k) {
        const unsigned v = r.ra();
        r.u32();
        if (v) {
            r.ra();
            r.skip(std::size_t{r.count(2)} * 2);
        }
        r.u32();
        r.ra();
        r.skip(r.count(1));
    }
    r.ra();
    r.skip(std::size_t{r.count(4)} * 4);
    {
        r.ra();
        const std::uint32_t n = r.count(4);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            r.ra();
            for (int k = 0; k < 2; ++k) {
                r.ra();
                r.u32();
                r.ra();
                r.skip(r.count(1));
            }
        }
    }
    if (version == 3) {
        node.extraMaterials.push_back(readMaterialList(r));
        node.extraMaterials.push_back(readMaterialList(r));
    } else if (version > 3) {
        r.ra();
        const std::uint32_t n = r.count(8);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i)
            node.extraMaterials.push_back(readMaterialList(r));
    }
    node.indexBuffers = readResList(r);
    node.vertexBuffers = readResList(r);
    node.textures = readResList(r);
    // Resource groups: u32, then three lists of (ResourceIndex, u32).
    r.ra();
    const std::uint32_t groups = r.count(16);
    for (std::uint32_t i = 0; i < groups && r.ok(); ++i) {
        r.ra();
        r.u32();
        for (int k = 0; k < 3; ++k) {
            r.ra();
            const std::uint32_t n = r.count(16);
            for (std::uint32_t j = 0; j < n && r.ok(); ++j) {
                r.ra();
                r.skip(12);
            }
        }
    }
    return r.ok();
}

/// A scene-node block: a CullNode and its RenderListNode_Commons (usually
/// one, sometimes two or none), each with its own geometry and buffer lists. nullopt
/// if the block at `p` isn't one.
std::optional<std::vector<SceneNode>> readSceneNode(ByteSpan d, std::size_t p, std::uint32_t block)
{
    Reader r(d, p);
    r.ra();
    r.ra();
    r.skip(8);
    r.ra();
    if (r.u32() != 0)
        return std::nullopt;
    r.ra();
    if (r.u32() != 0)
        return std::nullopt;
    r.ra();
    const std::uint32_t objects = r.count(8);
    std::vector<SceneNode> lists;
    for (std::uint32_t i = 0; i < objects && r.ok(); ++i) {
        if (r.get<std::uint8_t>() != 3)
            return std::nullopt;
        const std::string cls = r.str();
        if (cls == "RenderingData::CullNode") {
            skipCullNode(r, r.ra());
        } else if (cls == "RenderingData::RenderListNode_Common") {
            SceneNode& node = lists.emplace_back();
            node.block = block;
            if (!readRenderList(r, node))
                return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (!r.ok())
        return std::nullopt;
    return lists; // empty for a node with nothing to draw
}

// ---- data blocks: vertex and index buffers ----------------------------------------

struct VertexBuffer {
    std::uint32_t stride = 0, count = 0;
    std::size_t data = 0;   ///< File offset of the first vertex.
    int position = -1;      ///< Byte offsets of the elements (-1: none).
    int normal = -1;
    int uv = -1;
    int color = -1; ///< Four bytes; the last is the alpha (usage 9 or 10).
    std::uint32_t positionType = 0, normalType = 0, uvType = 0;
};

struct IndexBuffer {
    std::uint32_t count = 0;
    std::size_t data = 0;
};

/// One size of a texture: a complete picture with its own mip chain.
struct TextureLevel {
    image::PixelFormat format = image::PixelFormat::BC1;
    std::uint32_t width = 0, height = 0, mips = 0; ///< width 0: not stored at this level
    std::size_t data = 0;                          ///< File offset of the largest mip.
    std::uint32_t size = 0;
};

struct LevelTexture {
    std::string name; ///< "scene_vehicle_e3_diff"
    TextureLevel smallest; ///< The block's level 0 copy (the larger ones are in levels 1-9).
};

struct DataBlock {
    std::vector<VertexBuffer> vertexBuffers;
    std::vector<IndexBuffer> indexBuffers;
    std::vector<LevelTexture> textures;
};

/// "RA" 0, u32 size, u32 16, zero padding to a 16-byte boundary (counted from
/// the section's "ARCH") at least 5 bytes on, then the data.
std::optional<std::pair<std::size_t, std::uint32_t>> readBlob(Reader& r, ByteSpan d, std::size_t arch)
{
    const std::size_t at = r.pos();
    r.ra();
    const std::uint32_t size = r.u32();
    if (r.u32() != 16 || !r.ok())
        return std::nullopt;
    const std::size_t rel = at + 12 + 5 - arch;
    const std::size_t start = arch + rel + (16 - rel % 16) % 16;
    if (start > d.size() || d.size() - start < size)
        return std::nullopt;
    r.skip(start - r.pos() + size);
    return std::pair{start, size};
}

/// u32 present; then u32 28, width, height, depth, mip count, FourCC, -1, 3
/// and the pixels (every mip, largest first). nullopt if it can't be read.
std::optional<TextureLevel> readTextureLevel(Reader& r, ByteSpan d, std::size_t arch)
{
    TextureLevel t;
    if (r.u32() == 0)
        return r.ok() ? std::optional(t) : std::nullopt;
    r.u32();
    t.width = r.u32();
    t.height = r.u32();
    r.u32(); // depth
    t.mips = r.u32();
    const auto format = image::fromD3DFormat(r.u32());
    r.skip(8);
    const auto blob = readBlob(r, d, arch);
    if (!blob || !format || t.width == 0 || t.height == 0 || t.width > 8192 || t.height > 8192 || t.mips == 0
        || t.mips > 16)
        return std::nullopt;
    t.format = *format;
    t.data = blob->first;
    t.size = blob->second;
    if (image::surfaceSize(t.format, t.width, t.height) > t.size)
        return std::nullopt;
    return t;
}

/// Level 1-9 of a texture block: a larger copy of each of its textures (or none).
std::vector<TextureLevel> readTextureLevels(ByteSpan d, std::size_t p, std::size_t arch)
{
    Reader r(d, p);
    r.ra();
    r.ra();
    r.skip(8);
    std::vector<TextureLevel> out;
    const std::uint32_t n = r.count(16);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        r.ra();
        r.ra();
        r.u32();
        const auto level = readTextureLevel(r, d, arch);
        if (!level)
            break;
        out.push_back(*level);
    }
    return out;
}

std::optional<DataBlock> readDataBlock(ByteSpan d, std::size_t p, std::size_t arch)
{
    Reader r(d, p);
    r.ra();
    r.ra();
    r.skip(8);
    DataBlock block;
    r.ra();
    const std::uint32_t nvb = r.count(24);
    for (std::uint32_t i = 0; i < nvb && r.ok(); ++i) {
        r.get<std::uint8_t>(); // tag
        r.ra();
        VertexBuffer vb;
        r.ra();
        vb.stride = r.u32();
        r.ra();
        const std::uint32_t ne = r.count(20);
        std::vector<std::array<std::uint32_t, 3>> elements; // type, usage, usage index
        for (std::uint32_t e = 0; e < ne && r.ok(); ++e) {
            r.ra();
            const std::uint32_t type = r.u32();
            r.u32();
            const std::uint32_t usage = r.u32();
            elements.push_back({type, usage, r.u32()});
        }
        r.ra();
        const std::uint32_t no = r.count(2);
        std::vector<std::uint16_t> offsets;
        for (std::uint32_t o = 0; o < no && r.ok(); ++o)
            offsets.push_back(r.get<std::uint16_t>());
        vb.count = r.u32();
        const auto blob = readBlob(r, d, arch);
        if (!blob || offsets.size() != elements.size() || vb.stride == 0
            || std::uint64_t{vb.count} * vb.stride > blob->second)
            return std::nullopt;
        vb.data = blob->first;
        for (std::size_t e = 0; e < elements.size(); ++e) {
            const auto [type, usage, index] = elements[e];
            if (usage == 0 && vb.position < 0) {
                vb.position = offsets[e];
                vb.positionType = type;
            } else if (usage == 3 && vb.normal < 0) {
                vb.normal = offsets[e];
                vb.normalType = type;
            } else if (usage == 5 && index == 0 && vb.uv < 0) {
                vb.uv = offsets[e];
                vb.uvType = type;
            } else if ((usage == 9 || usage == 10) && (type == 0x0B || type == 0x04) && vb.color < 0) {
                vb.color = offsets[e];
            }
        }
        block.vertexBuffers.push_back(vb);
    }
    r.ra();
    const std::uint32_t nib = r.count(13);
    for (std::uint32_t i = 0; i < nib && r.ok(); ++i) {
        r.get<std::uint8_t>(); // tag
        r.ra();
        IndexBuffer ib;
        ib.count = r.u32();
        r.u32();
        const auto blob = readBlob(r, d, arch);
        if (!blob || std::uint64_t{ib.count} * 2 > blob->second)
            return std::nullopt;
        ib.data = blob->first;
        block.indexBuffers.push_back(ib);
    }
    if (!r.ok())
        return std::nullopt;
    // Then objects (none in data blocks) and textures: RA 2, RA 0, u32 2, name,
    // RA 1 + ten words, u32 hash, then its level 0 copy (the smallest).
    r.ra();
    if (r.u32() != 0)
        return block;
    const std::uint32_t nt = r.count(20);
    for (std::uint32_t i = 0; i < nt && r.ok(); ++i) {
        r.ra();
        r.ra();
        r.u32();
        LevelTexture t;
        t.name = r.str();
        if (r.ra() != 1)
            break;
        r.skip(40);
        r.u32(); // hash
        const auto level = readTextureLevel(r, d, arch);
        if (!level)
            break;
        t.smallest = *level;
        block.textures.push_back(std::move(t));
    }
    return block;
}

/// The textures the render file holds itself (Scene: race barriers, parked
/// cars, lamp posts, the environment cube…): what references to block
/// 0xFFFFFFFF name, by position. "RA" 0, "RA" 0, u32 2, u32 count, then per
/// texture: name, u32 hash, "RA" 2, "RA" 0, u32 2, name, "RA" 1 + ten words,
/// u32 hash, u32 size, then (size bytes) u32 width, height, layers, mip count,
/// FourCC, -1, u32 and the pixels.
std::vector<std::pair<std::string, TextureLevel>> readSharedTextures(ByteSpan scene, std::size_t sceneOffset)
{
    static constexpr std::array<std::byte, 12> head{std::byte{'R'}, std::byte{'A'}, std::byte{0}, std::byte{0},
                                                    std::byte{'R'}, std::byte{'A'}, std::byte{0}, std::byte{0},
                                                    std::byte{2},   std::byte{0},   std::byte{0}, std::byte{0}};
    std::vector<std::pair<std::string, TextureLevel>> out;
    for (auto it = scene.begin(); (it = std::search(it, scene.end(), head.begin(), head.end())) != scene.end(); ++it) {
        Reader r(scene, static_cast<std::size_t>(it - scene.begin()) + head.size());
        const std::uint32_t n = r.count(64);
        out.clear();
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            std::string name = r.str();
            r.u32();
            r.ra();
            r.ra();
            if (r.u32() != 2 || r.str() != name || r.ra() != 1)
                break;
            r.skip(40);
            r.u32();
            const std::uint32_t size = r.u32();
            const std::size_t end = r.pos() + size;
            TextureLevel t;
            t.width = r.u32();
            t.height = r.u32();
            const std::uint32_t layers = r.u32();
            t.mips = r.u32();
            const auto format = image::fromD3DFormat(r.u32());
            r.skip(8);
            if (!r.ok() || !format || end > scene.size() || size < 28)
                break;
            t.format = *format;
            t.data = sceneOffset + r.pos();
            t.size = size - 28;
            if (layers != 1 || image::surfaceSize(t.format, t.width, t.height) > t.size)
                t.width = 0; // a cube map (the environment): not a colour texture
            r.skip(end - r.pos());
            out.emplace_back(std::move(name), t);
        }
        if (n > 0 && out.size() == n)
            return out;
    }
    return {};
}

// ---- the stream's table of contents (end of the render file's Scene) --------------

struct Section {
    std::uint64_t offset = 0, size = 0;
    std::vector<ResIdx> blocks; ///< {block, detail level}, in file order.
};

std::optional<std::vector<Section>> readSectionTable(ByteSpan scene, std::size_t at, std::uint64_t streamSize)
{
    Reader r(scene, at);
    r.ra();
    r.ra();
    const std::uint32_t n = r.count(64);
    if (n == 0)
        return std::nullopt;
    std::vector<Section> table;
    std::uint64_t end = 0;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        Section s;
        r.ra();
        skipBounds(r);
        r.ra();
        const std::uint32_t nb = r.count(12);
        for (std::uint32_t b = 0; b < nb && r.ok(); ++b) {
            r.ra();
            const std::uint32_t block = r.u32();
            s.blocks.emplace_back(block, r.u32());
        }
        for (int k = 0; k < 3; ++k) {
            r.ra();
            r.skip(std::size_t{r.count(4)} * 4);
        }
        s.offset = r.get<std::uint64_t>();
        s.size = r.get<std::uint64_t>();
        r.get<std::uint64_t>();
        if (!r.ok() || s.offset < end || s.offset > streamSize || streamSize - s.offset < s.size)
            return std::nullopt;
        end = s.offset + s.size;
        table.push_back(std::move(s));
    }
    if (!r.ok())
        return std::nullopt;
    return table;
}

std::optional<std::vector<Section>> findSectionTable(ByteSpan scene, std::uint64_t streamSize)
{
    // "RA" 3, "RA" 0, u32 count, then the first entry: "RA" 1, "RA" 0 (its bounds)
    static constexpr std::array<std::byte, 4> head{std::byte{'R'}, std::byte{'A'}, std::byte{3}, std::byte{0}};
    for (std::size_t p = scene.size() >= 20 ? scene.size() - 20 : 0; p-- > 0;) {
        if (std::memcmp(scene.data() + p, head.data(), head.size()) != 0)
            continue;
        if (loadScalar<std::uint32_t>(scene, p + 4) != 0x4152 || loadScalar<std::uint32_t>(scene, p + 12) != 0x14152)
            continue;
        if (auto table = readSectionTable(scene, p, streamSize))
            return table;
    }
    return std::nullopt;
}

// ---- building the mesh ----------------------------------------------------------------

float readComponent(ByteSpan d, std::size_t at, std::uint32_t type, int k)
{
    switch (type) {
    case 0x06:
    case 0x07: return loadScalar<float>(d, at + 4 * static_cast<std::size_t>(k));
    case 0x08:
    case 0x09: return mesh::halfToFloat(loadScalar<std::uint16_t>(d, at + 2 * static_cast<std::size_t>(k)));
    case 0x0B: return std::to_integer<std::uint8_t>(d[at + static_cast<std::size_t>(k)]) / 127.5f - 1.0f;
    default: return 0.0f;
    }
}

/// The world matrix ("g_mWorld", 16 floats, row vectors) of a value set, or nullopt.
std::optional<std::array<float, 16>> worldMatrix(const SceneNode& node, ResIdx ref)
{
    if (ref.second != 0 || ref.first >= node.values.size())
        return std::nullopt;
    const ValueSet& set = node.values[ref.first];
    std::size_t at = 0;
    for (const MaterialElement& e : node.materials.types[set.type].elements) {
        if (e.name == "g_mWorld" && e.count == 16 && at + 16 <= set.words.size()) {
            std::array<float, 16> m{};
            for (std::size_t i = 0; i < 16; ++i)
                std::memcpy(&m[i], &set.words[at + i], 4);
            return m;
        }
        at += e.count;
    }
    return std::nullopt;
}

std::string effectOf(const SceneNode& node, ResIdx ref)
{
    if (ref.second != 0 || ref.first >= node.values.size())
        return {};
    return node.materials.types[node.values[ref.first].type].effect;
}

/// What became of one mesh (or one instance of it).
enum class Placed {
    Yes,
    NotShipped, ///< Its buffers are in a block the level's files don't hold.
    Invalid,    ///< Its data isn't understood.
};

class Builder {
public:
    Builder(ByteSpan stream, std::map<ResIdx, std::size_t> where, std::map<std::size_t, std::size_t> archOf,
            ByteSpan render, std::vector<std::pair<std::string, TextureLevel>> shared)
        : m_d(stream), m_render(render), m_where(std::move(where)), m_archOf(std::move(archOf)),
          m_shared(std::move(shared))
    {
        m_mesh.leftHanded = true;
    }

    const DataBlock* dataBlock(std::uint32_t block)
    {
        if (const auto it = m_blocks.find(block); it != m_blocks.end())
            return it->second ? &*it->second : nullptr;
        std::optional<DataBlock> parsed;
        if (const auto at = m_where.find({block, 0}); at != m_where.end())
            parsed = readDataBlock(m_d, at->second, m_archOf.at(at->second));
        auto& slot = m_blocks[block];
        slot = std::move(parsed);
        return slot ? &*slot : nullptr;
    }

    /// Appends one instance of `g` (placed by `world`) to the mesh's buffers.
    Placed addInstance(const SceneNode& node, const Geometry& g, const std::array<float, 16>& world)
    {
        if (g.streams.empty() || g.indexSlot >= node.indexBuffers.size()
            || g.streams.front().slot >= node.vertexBuffers.size())
            return Placed::Invalid;
        const ResIdx ibRef = node.indexBuffers[g.indexSlot];
        const ResIdx vbRef = node.vertexBuffers[g.streams.front().slot];
        // A few levels name blocks that aren't in their stream (a handful of
        // meshes; the multiplayer variants most).
        if (!m_where.contains({ibRef.first, 0}) || !m_where.contains({vbRef.first, 0}))
            return Placed::NotShipped;
        const DataBlock* ibBlock = dataBlock(ibRef.first);
        const DataBlock* vbBlock = dataBlock(vbRef.first);
        if (!ibBlock || !vbBlock || ibRef.second >= ibBlock->indexBuffers.size()
            || vbRef.second >= vbBlock->vertexBuffers.size())
            return Placed::Invalid;
        const IndexBuffer& ib = ibBlock->indexBuffers[ibRef.second];
        const VertexBuffer& vb = vbBlock->vertexBuffers[vbRef.second];
        if (vb.position < 0 || vb.stride == 0)
            return Placed::Invalid;
        const std::uint32_t base = g.streams.front().offset / vb.stride;
        for (const IndexRange& range : g.ranges) {
            if (std::uint64_t{range.start} + range.count > ib.count
                || std::uint64_t{base} + range.minIndex + range.vertices > vb.count)
                return Placed::Invalid;
            const auto first = static_cast<std::uint32_t>(m_mesh.vertexCount());
            for (std::uint32_t v = 0; v < range.vertices; ++v) {
                const std::size_t at = vb.data + std::size_t{base + range.minIndex + v} * vb.stride;
                float p[3], n[3] = {0, 0, 0};
                for (int k = 0; k < 3; ++k)
                    p[k] = readComponent(m_d, at + static_cast<std::size_t>(vb.position), vb.positionType, k);
                if (vb.normal >= 0)
                    for (int k = 0; k < 3; ++k)
                        n[k] = readComponent(m_d, at + static_cast<std::size_t>(vb.normal), vb.normalType, k);
                for (int k = 0; k < 3; ++k) {
                    const float wp = p[0] * world[k] + p[1] * world[4 + k] + p[2] * world[8 + k] + world[12 + k];
                    m_mesh.positions.push_back(std::isfinite(wp) ? wp : 0.0f);
                }
                float wn[3];
                for (int k = 0; k < 3; ++k)
                    wn[k] = n[0] * world[k] + n[1] * world[4 + k] + n[2] * world[8 + k];
                const float len = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                for (float c : wn)
                    m_mesh.normals.push_back(len > 1e-6f && std::isfinite(len) ? c / len : 0.0f);
                for (int k = 0; k < 2; ++k)
                    m_mesh.uvs.push_back(vb.uv >= 0 ? readComponent(m_d, at + static_cast<std::size_t>(vb.uv), vb.uvType, k)
                                                    : 0.0f);
                // Lightmapped meshes keep their lightmap coordinates in the
                // second half of a four-part texture coordinate.
                const bool lightmapUv = vb.uv >= 0 && (vb.uvType == 0x07 || vb.uvType == 0x09);
                for (int k = 2; k < 4; ++k)
                    m_mesh.uvs2.push_back(
                        lightmapUv ? readComponent(m_d, at + static_cast<std::size_t>(vb.uv), vb.uvType, k) : 0.0f);
                for (std::size_t k = 0; k < 4; ++k)
                    m_mesh.colors.push_back(
                        vb.color >= 0 ? std::to_integer<std::uint8_t>(m_d[at + static_cast<std::size_t>(vb.color) + k])
                                      : std::uint8_t{255});
            }
            // Indices (absolute in the buffer) → this copy's vertices.
            const auto index = [&](std::uint32_t i, std::uint32_t& out) {
                const std::uint32_t v = loadScalar<std::uint16_t>(m_d, ib.data + 2 * std::size_t{i});
                if (v < range.minIndex || v - range.minIndex >= range.vertices)
                    return false;
                out = first + (v - range.minIndex);
                return true;
            };
            if (g.primitive == 1) { // triangle strip, stitched with degenerate triangles
                for (std::uint32_t i = 2; i < range.count; ++i) {
                    std::uint32_t a, b, c;
                    if (!index(range.start + i - 2, a) || !index(range.start + i - 1, b) || !index(range.start + i, c))
                        continue;
                    if (a == b || b == c || a == c)
                        continue;
                    if (i % 2)
                        std::swap(a, b);
                    m_mesh.indices.insert(m_mesh.indices.end(), {a, b, c});
                }
            } else {
                for (std::uint32_t i = 0; i + 2 < range.count; i += 3) {
                    std::uint32_t a, b, c;
                    if (index(range.start + i, a) && index(range.start + i + 1, b) && index(range.start + i + 2, c))
                        m_mesh.indices.insert(m_mesh.indices.end(), {a, b, c});
                }
            }
        }
        return Placed::Yes;
    }

    /// The mesh texture for a node's texture reference (block, index): the
    /// largest copy up to kMaxTextureSize, its pixels kept as stored. -1 when
    /// the level doesn't hold it (the game's own: environment maps…).
    int texture(ResIdx ref)
    {
        if (const auto it = m_textureOf.find(ref); it != m_textureOf.end())
            return it->second;
        int& index = m_textureOf[ref];
        index = -1;
        if (ref.first == kSharedBlock) { // the render file's own
            if (ref.second >= m_shared.size() || m_shared[ref.second].second.width == 0)
                return -1;
            const auto& [name, level] = m_shared[ref.second];
            const auto first = m_render.begin() + static_cast<std::ptrdiff_t>(level.data);
            index = addTexture(name, level, ByteBuffer(first, first + level.size), level.mips);
            return index;
        }
        const DataBlock* block = m_where.contains({ref.first, 0}) ? dataBlock(ref.first) : nullptr;
        if (!block || ref.second >= block->textures.size())
            return -1;
        // Level 0 holds the small sizes (a whole mip chain); each higher level
        // one larger size on its own. Put the chain back together, largest
        // first, from the biggest size up to kMaxTextureSize.
        const LevelTexture& t = block->textures[ref.second];
        std::vector<TextureLevel> copies{t.smallest};
        for (std::uint32_t level = 1; level < 10; ++level)
            if (const auto at = m_where.find({ref.first, level}); at != m_where.end()) {
                const auto list = readTextureLevels(m_d, at->second, m_archOf.at(at->second));
                if (ref.second < list.size() && list[ref.second].width != 0)
                    copies.push_back(list[ref.second]);
            }
        TextureLevel chain;
        ByteBuffer pixels;
        std::uint32_t w = 0, h = 0;
        for (const TextureLevel& c : copies)
            if (c.width != 0 && c.width <= kMaxTextureSize && c.height <= kMaxTextureSize && c.width > w) {
                chain = c;
                w = c.width;
                h = c.height;
            }
        if (w == 0)
            return -1;
        std::uint32_t mips = 0;
        while (true) {
            const auto copy = std::ranges::find_if(copies, [&](const TextureLevel& c) { return c.width == w && c.height == h; });
            if (copy == copies.end())
                break;
            // As many sizes as the copy holds (one, or the rest of the chain).
            std::size_t at = copy->data, left = copy->size;
            bool more = true;
            while (more) {
                const std::size_t size = image::surfaceSize(chain.format, w, h);
                if (size > left)
                    break;
                pixels.insert(pixels.end(), m_d.begin() + static_cast<std::ptrdiff_t>(at),
                              m_d.begin() + static_cast<std::ptrdiff_t>(at + size));
                at += size;
                left -= size;
                ++mips;
                more = w > 1 || h > 1;
                w = std::max(1u, w / 2);
                h = std::max(1u, h / 2);
            }
            if (!more)
                break;
        }
        index = addTexture(t.name, chain, pixels, mips);
        return index;
    }

    int addTexture(const std::string& name, const TextureLevel& level, ByteBuffer pixels, std::uint32_t mips)
    {
        Mesh::Texture out;
        out.name = name;
        out.stored.format = level.format;
        out.stored.width = level.width;
        out.stored.height = level.height;
        out.stored.mipCount = mips;
        out.stored.data = std::move(pixels);
        m_mesh.textures.push_back(std::move(out));
        return static_cast<int>(m_mesh.textures.size()) - 1;
    }

    /// The colour textures and the lightmap a geometry binds: the texture
    /// elements of its streams' value sets (the first stream's is the
    /// material; a second one, if any, holds just "g_tLightmap"). They index
    /// the node's texture list.
    std::vector<Mesh::TextureSlot> textureSlots(const SceneNode& node, const Geometry& g)
    {
        static constexpr std::string_view kWanted[] = {"g_tAlbedo",     "g_tColour", "g_tTexture",
                                                       "g_2L_tTexture", "g_tNeon",   "g_tLightmap"};
        std::vector<Mesh::TextureSlot> slots;
        for (const Stream& stream : g.streams) {
            const ResIdx ref = stream.values;
            if (ref.second != 0 || ref.first >= node.values.size())
                continue;
            const ValueSet& set = node.values[ref.first];
            std::size_t at = 0;
            for (const MaterialElement& e : node.materials.types[set.type].elements) {
                if (e.kind == 7 && e.count == 1 && at < set.words.size()
                    && std::ranges::find(kWanted, std::string_view(e.name)) != std::end(kWanted)
                    && set.words[at] < node.textures.size()
                    && std::ranges::none_of(slots, [&](const Mesh::TextureSlot& s) { return s.name == e.name; }))
                    slots.push_back({e.name, texture(node.textures[set.words[at]])});
                at += e.count;
            }
        }
        return slots;
    }

    Mesh& mesh() noexcept { return m_mesh; }

private:
    ByteSpan m_d;
    ByteSpan m_render;
    std::map<ResIdx, std::size_t> m_where;     ///< (block, level) → file offset of its header
    std::map<std::size_t, std::size_t> m_archOf; ///< block header offset → its section's start
    std::unordered_map<std::uint32_t, std::optional<DataBlock>> m_blocks;
    std::map<ResIdx, int> m_textureOf;
    std::vector<std::pair<std::string, TextureLevel>> m_shared; ///< The render file's own textures.
    Mesh m_mesh;
};

} // namespace

Result<Mesh> decodeLevel(ByteSpan render, ByteSpan stream)
{
    // "  CPRender" { Header, Scene, LS, Lights, PVSData, CubePt2 }: walk its
    // children up to the Scene (the last ones don't parse as a tidy sequence).
    const auto root = readChunkHeader(render, 0);
    if (!root || root->name != "  CPRender")
        return fail(ErrorCode::InvalidFormat, "not a level render file");
    std::optional<Chunk> scene;
    for (std::size_t p = root->payloadOffset(); p < render.size();) {
        auto chunk = readChunkHeader(render, p);
        if (!chunk || chunk->name == "Scene") {
            scene = std::move(chunk);
            break;
        }
        p += chunk->size;
    }
    if (!scene)
        return fail(ErrorCode::InvalidFormat, "no Scene chunk in the level's render file");
    const ByteSpan sceneData = render.subspan(scene->payloadOffset(), scene->payloadSize());
    const auto table = findSectionTable(sceneData, stream.size());
    if (!table)
        return fail(ErrorCode::InvalidFormat, "the level's render file has no table of the stream's sections");

    // Where each (block, level) starts: its header, in the order the table gives.
    std::map<ResIdx, std::size_t> where;
    std::map<std::size_t, std::size_t> archOf;
    for (const Section& s : *table) {
        std::size_t cursor = static_cast<std::size_t>(s.offset);
        const std::size_t end = static_cast<std::size_t>(s.offset + s.size);
        for (const ResIdx& b : s.blocks) {
            std::array<std::byte, 16> head{};
            std::memcpy(head.data(), "RA\0\0RA\0\0", 8);
            const std::uint32_t two = 2;
            std::memcpy(head.data() + 8, &two, 4);
            std::memcpy(head.data() + 12, &b.second, 4);
            const auto it = std::search(stream.begin() + static_cast<std::ptrdiff_t>(cursor),
                                        stream.begin() + static_cast<std::ptrdiff_t>(end), head.begin(), head.end());
            if (it == stream.begin() + static_cast<std::ptrdiff_t>(end))
                return fail(ErrorCode::InvalidFormat, "block {} (level {}) not found in its section at {:#x}", b.first,
                            b.second, s.offset);
            cursor = static_cast<std::size_t>(it - stream.begin());
            where[b] = cursor;
            archOf[cursor] = static_cast<std::size_t>(s.offset);
            cursor += head.size();
        }
    }

    Builder builder(stream, where, archOf, render, readSharedTextures(sceneData, scene->payloadOffset()));
    std::size_t nodes = 0, unread = 0, skipped = 0;
    std::array<std::size_t, 3> placed{}; // by Placed
    for (const Section& s : *table) {
        for (const ResIdx& b : s.blocks) {
            if (b.second != 0)
                continue;
            const auto lists = readSceneNode(stream, where.at(b), b.first);
            if (!lists) {
                // Data blocks fail here too; only a block that starts like a
                // scene node counts as one not understood.
                if (const auto p = where.at(b) + 45; p + 23 <= stream.size()
                    && std::memcmp(stream.data() + p, "RenderingData::CullNode", 23) == 0)
                    ++unread;
                continue;
            }
            ++nodes;
            for (const SceneNode& renderList : *lists) {
                const SceneNode* const node = &renderList;
                const auto addPart = [&](std::uint32_t first, std::string name, std::string shader,
                                         std::vector<Mesh::TextureSlot> slots, Mesh::Part::Alpha alpha) {
                    Mesh& mesh = builder.mesh();
                    const auto count = static_cast<std::uint32_t>(mesh.indices.size()) - first;
                    if (count == 0)
                        return;
                    Mesh::Part part;
                    part.name = std::move(name);
                    part.group = std::format("Node {}", node->block);
                    part.shader = std::move(shader);
                    part.material = part.shader;
                    part.textures = std::move(slots);
                    part.alpha = alpha;
                    part.lods.push_back({first, count});
                    mesh.parts.push_back(std::move(part));
                };
                // A geometry's material: its effect and the colour textures it binds.
                const auto materialOf = [&](const Geometry& g) {
                    std::pair<std::string, std::vector<Mesh::TextureSlot>> m;
                    if (!g.streams.empty()) {
                        m.first = effectOf(*node, g.streams.front().values);
                        m.second = builder.textureSlots(*node, g);
                    }
                    return m;
                };
                const auto textureName = [&](const std::vector<Mesh::TextureSlot>& slots) {
                    for (const Mesh::TextureSlot& slot : slots)
                        if (slot.texture >= 0 && slot.name != "g_tLightmap")
                            return builder.mesh().textures[static_cast<std::size_t>(slot.texture)].name;
                    return std::string{};
                };
                // The world itself (already in place): one part per material.
                struct Group {
                    std::vector<Mesh::TextureSlot> slots;
                    std::vector<const Geometry*> geometry;
                };
                std::map<std::tuple<std::string, int, Mesh::Part::Alpha, int>, Group> byMaterial;
                for (const Geometry& g : node->staticGeometry) {
                    auto [effect, slots] = materialOf(g);
                    int colour = -1, lightmap = -1;
                    for (const Mesh::TextureSlot& slot : slots) {
                        if (slot.name == "g_tLightmap")
                            lightmap = slot.texture;
                        else if (colour == -1)
                            colour = slot.texture;
                    }
                    Group& group = byMaterial[{std::move(effect), colour, g.alpha, lightmap}];
                    group.slots = std::move(slots);
                    group.geometry.push_back(&g);
                }
                for (auto& [key, group] : byMaterial) {
                    const auto first = static_cast<std::uint32_t>(builder.mesh().indices.size());
                    for (const Geometry* g : group.geometry)
                        ++placed[static_cast<std::size_t>(builder.addInstance(*node, *g, kIdentity))];
                    const std::string& effect = std::get<0>(key);
                    std::string name = effect.empty() ? std::string("World") : effect;
                    if (name.ends_with(".fx"))
                        name.resize(name.size() - 3);
                    if (const std::string texture = textureName(group.slots); !texture.empty())
                        name = std::format("{} · {}", texture, name);
                    addPart(first, std::move(name), effect, std::move(group.slots), std::get<2>(key));
                }
                // Props: one part per mesh, with every instance of it placed.
                std::vector<std::vector<const Draw*>> byGeometry(node->geometry.size());
                for (const Draw& d : node->draws)
                    if (d.geometry < byGeometry.size())
                        byGeometry[d.geometry].push_back(&d);
                    else
                        ++skipped;
                for (std::size_t gi = 0; gi < byGeometry.size(); ++gi) {
                    if (byGeometry[gi].empty())
                        continue;
                    const auto first = static_cast<std::uint32_t>(builder.mesh().indices.size());
                    const Geometry& g = node->geometry[gi];
                    for (const Draw* d : byGeometry[gi]) {
                        if (const auto world = worldMatrix(*node, d->instance))
                            ++placed[static_cast<std::size_t>(builder.addInstance(*node, g, *world))];
                        else
                            ++skipped;
                    }
                    auto [effect, slots] = materialOf(g);
                    const std::string texture = textureName(slots);
                    addPart(first,
                            texture.empty() ? std::format("Prop {} ({}×)", gi, byGeometry[gi].size())
                                            : std::format("Prop {} ({}×) · {}", gi, byGeometry[gi].size(), texture),
                            std::move(effect), std::move(slots), g.alpha);
                }
            }
        }
    }
    Mesh& mesh = builder.mesh();
    if (mesh.parts.empty())
        return fail(ErrorCode::InvalidFormat, "no drawable geometry found in the level ({} scene nodes)", nodes);
    skipped += placed[static_cast<std::size_t>(Placed::Invalid)];
    mesh.notes.push_back(std::format("{} sections, {} scene nodes, {} meshes placed, {} textures", table->size(),
                                     nodes, placed[static_cast<std::size_t>(Placed::Yes)], mesh.textures.size()));
    if (const std::size_t missing = placed[static_cast<std::size_t>(Placed::NotShipped)])
        mesh.notes.push_back(std::format("{} mesh(es) not drawn: their data isn't in the level's files", missing));
    if (unread)
        mesh.notes.push_back(std::format("{} scene node(s) skipped (layout not understood)", unread));
    if (skipped)
        mesh.notes.push_back(std::format("{} draw(s) skipped (data not understood)", skipped));
    return std::move(mesh);
}

bool LevelDecoder::canDecode(std::string_view extension, ByteSpan) const
{
    return extension == "levelrender" || extension == "levelstream";
}

Result<Mesh> LevelDecoder::decode(ByteSpan) const
{
    return fail(ErrorCode::Unsupported, "a level is shown from its folder (Level.levelrender with Level.levelstream)");
}

Result<Mesh> LevelDecoder::decodeWith(ByteSpan, const DecompileContext& context) const
{
    if (!context.readSibling)
        return decode({});
    // The preview may hold only the start of the big stream file: read both whole.
    auto render = context.readSibling("Level.levelrender");
    auto stream = context.readSibling("Level.levelstream");
    if (!render || !stream)
        return fail(ErrorCode::NotFound, "a level needs both Level.levelrender and Level.levelstream in its folder");
    return decodeLevel(*render, *stream);
}

} // namespace viewer::games::blur
