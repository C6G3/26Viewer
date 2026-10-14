// Blur physics streams: collision shapes.
//
// Models keep theirs in a "Coll" chunk (cars: a body per moving part — wheels,
// doors, bumpers… — plus the car's own colliders; props: boxes, cylinders and
// hulls); levels in .levelphysics files: a "Main" chunk (track and walls) and
// one "Route" chunk per route layout. All use the same stream, read by the
// game's physics loader:
//
//   "Head", u32 version (10; a few props 3, 6 or 7), objects until type 0x0FFFFFFF:
//   u32 type, the object's fields, then its placement: f32 position[3],
//   f32 rotation[9] (rows: p' = p * R + position), f32 inertia[9].
//
// Every object starts with 13 common words (12 before version 7): f32, f32,
// u32, u32 material (version >= 2), u32 flags, u32, f32, u32 handle,
// f32 offset[3], u32, u32 (version >= 7). A car part's body has handle
// 0xC00000nn, the render tag of the model element it moves with. `offset` is
// the centre of mass; convex hull points are stored around their body's
// (a hull inside a body has an offset of its own, which isn't used for that). The material
// is a surface type (see surfaceName); triangle meshes have one per triangle.
//
// Types and their fields (sizes and radii need version >= 4):
//   0, 11  triangle mesh (see readMesh); 11 is a template that type 10 places
//   17     f32, triangle mesh, then (version >= 5) 8 words
//   2      box: f32 half extents[3]
//   3      convex hull: u32 nv, u32 nt, f32 points[nv][3], u32 corners[nt][3]
//   4      sphere: f32 radius
//   5      capsule: f32 height, f32 radius (along y)
//   6      cylinder: f32 height, f32 radius (along y)
//   15     cone: f32 radius, f32 height (along y)
//   7      body: u32 count, u32 (version >= 6), then count shapes: u32 type, its
//          fields, f32, f32 rotation[9], f32 position[3], f32[9]
//          (its place in the body)
//   10     a copy of the shape with handle u32 (version >= 4), placed by its
//          own placement words
// Types and sizes come from the game's loader; volume formulas tell the shapes
// apart (π r² h for cylinders, π r² h / 3 for cones…).

#include "Physics.hpp"

#include "ChunkFile.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <numbers>

namespace viewer::games::blur {

namespace {

constexpr std::uint32_t kEnd = 0x0FFFFFFF;

/// Little-endian reads that stop at the end: a read past it clears `ok()`
/// and returns 0, so a damaged stream can't run off its buffer.
class Stream {
public:
    explicit Stream(ByteSpan data) : m_data(data) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_data.size(); }
    [[nodiscard]] ByteSpan data() const noexcept { return m_data; }

    bool need(std::size_t bytes)
    {
        if (m_ok && remaining() >= bytes)
            return true;
        m_ok = false;
        return false;
    }
    std::uint32_t u32()
    {
        if (!need(4))
            return 0;
        const auto value = loadScalar<std::uint32_t>(m_data, m_pos);
        m_pos += 4;
        return value;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    void skip(std::size_t bytes)
    {
        if (need(bytes))
            m_pos += bytes;
    }
    void seek(std::size_t pos)
    {
        if (pos <= m_data.size())
            m_pos = pos;
        else
            m_ok = false;
    }

private:
    ByteSpan m_data;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

/// Rotation rows and a translation: p' = p * R + t.
struct Placement {
    std::array<float, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> t{0, 0, 0};

    void apply(float* p) const noexcept
    {
        const float x = p[0], y = p[1], z = p[2];
        p[0] = x * r[0] + y * r[3] + z * r[6] + t[0];
        p[1] = x * r[1] + y * r[4] + z * r[7] + t[1];
        p[2] = x * r[2] + y * r[5] + z * r[8] + t[2];
    }
    /// This placement, then `outer`.
    [[nodiscard]] Placement then(const Placement& outer) const noexcept
    {
        Placement out;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                out.r[3 * row + col] = r[3 * row] * outer.r[col] + r[3 * row + 1] * outer.r[3 + col]
                                       + r[3 * row + 2] * outer.r[6 + col];
        out.t = t;
        outer.apply(out.t.data());
        return out;
    }
};

Placement readRotationThenPosition(Stream& s)
{
    Placement p;
    for (float& v : p.r)
        v = s.f32();
    for (float& v : p.t)
        v = s.f32();
    if (!std::ranges::all_of(p.r, [](float v) { return std::isfinite(v); })
        || !std::ranges::all_of(p.t, [](float v) { return std::isfinite(v); }))
        return {};
    return p;
}

/// An object's placement words: position, rotation, inertia.
Placement readObjectPlacement(Stream& s)
{
    Placement p;
    for (float& v : p.t)
        v = s.f32();
    for (float& v : p.r)
        v = s.f32();
    s.skip(9 * 4); // inertia
    if (!std::ranges::all_of(p.r, [](float v) { return std::isfinite(v); })
        || !std::ranges::all_of(p.t, [](float v) { return std::isfinite(v); }))
        return {};
    return p;
}

struct Base {
    std::uint32_t handle = 0;
    std::uint32_t material = 0;           ///< Surface type.
    std::array<float, 3> offset{0, 0, 0}; ///< Centre of mass.
};

Base readBase(Stream& s, std::uint32_t version)
{
    Base b;
    s.skip(8); // two floats
    s.u32();
    if (version >= 2)
        b.material = s.u32();
    s.u32(); // flags
    s.u32();
    s.f32();
    b.handle = s.u32();
    for (float& v : b.offset)
        v = s.f32();
    s.u32();
    if (version >= 7)
        s.u32();
    if (!std::ranges::all_of(b.offset, [](float v) { return std::isfinite(v); }))
        b.offset = {0, 0, 0};
    return b;
}

/// Geometry of one shape in its own space.
struct Geometry {
    std::string kind;
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint16_t> surfaces;
    std::uint32_t handle = 0;
    std::uint32_t copyOf = 0; ///< Type 10: handle of the shape it repeats.
    bool drawable = false;
    std::uint16_t material = 0;
};

void addQuad(std::vector<std::uint32_t>& out, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
    out.insert(out.end(), {a, b, c, a, c, d});
}

Geometry box(float hx, float hy, float hz)
{
    Geometry g{"Box", {}, {}, {}, 0, 0, true};
    for (int i = 0; i < 8; ++i)
        g.positions.insert(g.positions.end(), {i & 1 ? hx : -hx, i & 2 ? hy : -hy, i & 4 ? hz : -hz});
    addQuad(g.indices, 0, 1, 3, 2); // -z
    addQuad(g.indices, 4, 6, 7, 5); // +z
    addQuad(g.indices, 0, 4, 5, 1); // -y
    addQuad(g.indices, 2, 3, 7, 6); // +y
    addQuad(g.indices, 0, 2, 6, 4); // -x
    addQuad(g.indices, 1, 5, 7, 3); // +x
    return g;
}

/// A solid of revolution around y: rings of (radius, y), closed at both ends.
Geometry lathe(std::string kind, const std::vector<std::pair<float, float>>& rings)
{
    constexpr int kSides = 16;
    Geometry g{std::move(kind), {}, {}, {}, 0, 0, true};
    for (const auto& [radius, y] : rings)
        for (int i = 0; i < kSides; ++i) {
            const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kSides;
            g.positions.insert(g.positions.end(), {radius * std::cos(a), y, radius * std::sin(a)});
        }
    const auto ringStart = [](std::size_t ring) { return static_cast<std::uint32_t>(ring * kSides); };
    for (std::size_t ring = 0; ring + 1 < rings.size(); ++ring)
        for (std::uint32_t i = 0; i < kSides; ++i) {
            const std::uint32_t j = (i + 1) % kSides;
            addQuad(g.indices, ringStart(ring) + i, ringStart(ring) + j, ringStart(ring + 1) + j, ringStart(ring + 1) + i);
        }
    // caps: fans around the end rings' centres
    for (const std::size_t ring : {std::size_t{0}, rings.size() - 1}) {
        if (rings[ring].first <= 0.0f)
            continue;
        const auto centre = static_cast<std::uint32_t>(g.positions.size() / 3);
        g.positions.insert(g.positions.end(), {0.0f, rings[ring].second, 0.0f});
        for (std::uint32_t i = 0; i < kSides; ++i)
            g.indices.insert(g.indices.end(), {centre, ringStart(ring) + i, ringStart(ring) + (i + 1) % kSides});
    }
    return g;
}

Geometry sphere(float r, float halfHeight, std::string kind)
{
    // Rings from the bottom pole to the top; the middle is stretched by halfHeight (capsules).
    std::vector<std::pair<float, float>> rings;
    constexpr int kSteps = 8;
    for (int i = 0; i <= kSteps; ++i) {
        const float a = std::numbers::pi_v<float> * (static_cast<float>(i) / kSteps - 0.5f);
        const float y = r * std::sin(a) + (i < kSteps / 2 ? -halfHeight : i > kSteps / 2 ? halfHeight : 0.0f);
        rings.emplace_back(r * std::cos(a), y);
        if (i == kSteps / 2 && halfHeight > 0.0f) { // the straight middle: two rings
            rings.back().second = -halfHeight;
            rings.emplace_back(r, halfHeight);
        }
    }
    return lathe(std::move(kind), rings);
}

/// Triangle mesh:
///   u32 vertexCount, u32 n, u32 bytes (in memory), u32 nodeCount, f32 bounds[6],
///   f32 vertices[vertexCount][3], u32[n],
///   nodes[nodeCount], 28 bytes: u16 box[6], u32 triangles, u32 groupOffset
///     (where the leaf's triangles sit in memory), u32 firstVertex, u32 vertices,
///   the leaves' triangles in groupOffset order, in groups: 15 bytes (u16 at +6:
///     surface type; u8 at +14: count), then count x 11 bytes (u8 corners[3],
///     relative to the leaf's first vertex; u32 edge flags; u32),
///   version >= 7: u32 n, n x 12 bytes.
Result<Geometry> readMesh(Stream& s, std::uint32_t version)
{
    Geometry g{"Triangle mesh", {}, {}, {}, 0, 0, true};
    const std::uint32_t vertexCount = s.u32(), extra = s.u32();
    s.u32();
    const std::uint32_t nodeCount = s.u32();
    s.skip(6 * 4);
    if (!s.ok()
        || std::uint64_t{vertexCount} * 12 + std::uint64_t{extra} * 4 + std::uint64_t{nodeCount} * 28 > s.remaining())
        return fail(ErrorCode::InvalidFormat, "collision mesh larger than its data ({} vertices, {} nodes)", vertexCount,
                    nodeCount);
    g.positions.resize(std::size_t{vertexCount} * 3);
    for (float& v : g.positions)
        if (v = s.f32(); !std::isfinite(v))
            v = 0.0f;
    s.skip(std::size_t{extra} * 4);

    struct Leaf {
        std::uint32_t triangles = 0, offset = 0, first = 0, count = 0;
    };
    std::vector<Leaf> leaves;
    for (std::uint32_t n = 0; n < nodeCount; ++n) {
        s.skip(12); // quantised box
        Leaf leaf;
        leaf.triangles = s.u32();
        leaf.offset = s.u32();
        leaf.first = s.u32();
        leaf.count = s.u32();
        if (leaf.triangles)
            leaves.push_back(leaf);
    }
    std::ranges::stable_sort(leaves, {}, &Leaf::offset);
    const auto broken = [&] { return fail(ErrorCode::InvalidFormat, "collision mesh triangles damaged near byte {}", s.position()); };
    for (const Leaf& leaf : leaves) {
        if (std::uint64_t{leaf.first} + leaf.count > vertexCount)
            return broken();
        for (std::uint32_t left = leaf.triangles; left > 0;) {
            if (!s.need(15))
                return broken();
            const auto surface = loadScalar<std::uint16_t>(s.data(), s.position() + 6);
            const auto count = std::to_integer<std::uint32_t>(s.data()[s.position() + 14]);
            if (count == 0 || count > left || !s.need(15 + std::size_t{count} * 11))
                return broken();
            s.skip(15);
            for (std::uint32_t t = 0; t < count; ++t) {
                const std::size_t at = s.position();
                std::array<std::uint32_t, 3> corner{};
                for (std::size_t k = 0; k < 3; ++k)
                    corner[k] = std::to_integer<std::uint32_t>(s.data()[at + k]);
                if (std::ranges::max(corner) >= leaf.count)
                    return broken();
                g.indices.insert(g.indices.end(), {leaf.first + corner[0], leaf.first + corner[1], leaf.first + corner[2]});
                g.surfaces.push_back(surface);
                s.skip(11);
            }
            left -= count;
        }
    }
    if (version >= 7)
        s.skip(std::size_t{s.u32()} * 12);
    return g;
}

/// Reads one shape's fields (after its type word). Bodies (type 7) add their
/// shapes to `children` with their places in the body; other shapes are returned.
/// `body` is the base of the body the shape belongs to (null: a shape of its own).
Result<Geometry> readShape(Stream& s, std::uint32_t type, std::uint32_t version, std::vector<Geometry>& children,
                           std::vector<Placement>& childPlacements, std::size_t depth, const Base* body = nullptr)
{
    const Base base = readBase(s, version);
    // Convex hull points are stored around a centre of mass: a hull of its own
    // around its own; one in a body (a car's bonnet, door, wing…) around the
    // body's, which is added after the hull's place in the body has turned it.
    const std::array<float, 3> centre = body ? std::array<float, 3>{0, 0, 0} : base.offset;
    const bool sized = version >= 4;
    Geometry g;
    switch (type) {
    case 0:
    case 11:
    case 17: {
        if (type == 17)
            s.f32();
        auto mesh = readMesh(s, version);
        if (!mesh)
            return std::unexpected(mesh.error());
        g = std::move(*mesh);
        if (type == 17 && version >= 5)
            s.skip(8 * 4);
        break;
    }
    case 2: {
        const float hx = s.f32(), hy = s.f32(), hz = s.f32();
        g = box(hx, hy, hz);
        break;
    }
    case 3: {
        const std::uint32_t nv = s.u32(), nt = s.u32();
        if (!s.ok() || (std::uint64_t{nv} + nt) * 12 > s.remaining())
            return fail(ErrorCode::InvalidFormat, "convex hull too large ({} points, {} triangles)", nv, nt);
        g = Geometry{"Convex hull", {}, {}, {}, 0, 0, true};
        g.positions.resize(std::size_t{nv} * 3);
        for (std::size_t i = 0; i < g.positions.size(); ++i) {
            const float v = s.f32() + centre[i % 3];
            g.positions[i] = std::isfinite(v) ? v : 0.0f;
        }
        g.indices.resize(std::size_t{nt} * 3);
        for (std::uint32_t& i : g.indices)
            if ((i = s.u32()) >= nv)
                return fail(ErrorCode::InvalidFormat, "convex hull corner {} out of range ({} points)", i, nv);
        break;
    }
    case 4: {
        const float r = sized ? s.f32() : 0.0f;
        g = sphere(r, 0.0f, "Sphere");
        break;
    }
    case 5: {
        const float h = sized ? s.f32() : 0.0f, r = sized ? s.f32() : 0.0f;
        g = sphere(r, h / 2, "Capsule");
        break;
    }
    case 6: {
        const float h = sized ? s.f32() : 0.0f, r = sized ? s.f32() : 0.0f;
        g = lathe("Cylinder", {{r, -h / 2}, {r, h / 2}});
        break;
    }
    case 15: {
        const float r = sized ? s.f32() : 0.0f, h = sized ? s.f32() : 0.0f;
        g = lathe("Cone", {{r, -h / 2}, {0.0f, h / 2}});
        break;
    }
    case 7: {
        if (depth > 8)
            return fail(ErrorCode::InvalidFormat, "bodies nested too deeply");
        const std::uint32_t count = s.u32();
        if (version >= 6)
            s.u32();
        if (count > 4096)
            return fail(ErrorCode::InvalidFormat, "body with {} shapes", count);
        for (std::uint32_t i = 0; i < count && s.ok(); ++i) {
            const std::uint32_t childType = s.u32();
            std::vector<Geometry> grand;
            std::vector<Placement> grandPlacements;
            auto child = readShape(s, childType, version, grand, grandPlacements, depth + 1, &base);
            if (!child)
                return std::unexpected(child.error());
            s.f32(); // in every version
            Placement place = readRotationThenPosition(s);
            if (childType == 3)
                for (int k = 0; k < 3; ++k)
                    place.t[static_cast<std::size_t>(k)] += base.offset[static_cast<std::size_t>(k)];
            s.skip(9 * 4);
            for (std::size_t k = 0; k < grand.size(); ++k) {
                children.push_back(std::move(grand[k]));
                childPlacements.push_back(grandPlacements[k].then(place));
            }
            children.push_back(std::move(*child));
            childPlacements.push_back(place);
        }
        g.kind = "Body";
        break;
    }
    case 10:
        g.kind = "Copy";
        g.copyOf = sized ? s.u32() : 0;
        break;
    default:
        return fail(ErrorCode::Unsupported, "unknown collision shape type {} at byte {}", type, s.position());
    }
    if (!s.ok())
        return fail(ErrorCode::InvalidFormat, "collision data ends inside a shape");
    if (g.positions.empty() || g.indices.empty())
        g.drawable = false;
    g.handle = base.handle;
    g.material = static_cast<std::uint16_t>(std::min<std::uint32_t>(base.material, 0xFFFF));
    return g;
}

void place(Geometry& g, const Placement& p)
{
    for (std::size_t i = 0; i + 2 < g.positions.size(); i += 3)
        p.apply(&g.positions[i]);
}

} // namespace

Result<CollisionSet> readPhysicsStream(ByteSpan data)
{
    Stream s(data);
    if (data.size() < 8 || data[0] != std::byte{'H'} || data[1] != std::byte{'e'} || data[2] != std::byte{'a'}
        || data[3] != std::byte{'d'})
        return fail(ErrorCode::InvalidFormat, "not a physics stream (no \"Head\")");
    s.skip(4);
    const std::uint32_t version = s.u32();
    if (version == 0 || version > 32)
        return fail(ErrorCode::Unsupported, "physics stream version {}", version);

    CollisionSet set;
    std::map<std::uint32_t, Geometry> templates; // type 11 shapes, by handle, for type 10 copies
    std::vector<std::uint32_t> templateObjects;
    // Streams end with type 0x0FFFFFFF (a few at the end of their file just stop).
    while (!s.atEnd()) {
        const std::uint32_t type = s.u32();
        if (!s.ok() || type == kEnd)
            break;
        std::vector<Geometry> children;
        std::vector<Placement> childPlacements;
        auto shape = readShape(s, type, version, children, childPlacements, 0);
        // (bodies return their shapes in `children`, already placed within the body)
        if (!shape)
            return std::unexpected(shape.error());
        const Placement placement = readObjectPlacement(s);
        if (!s.ok())
            return fail(ErrorCode::InvalidFormat, "collision data ends inside an object");
        const std::size_t object = set.objects++;

        const auto emit = [&](Geometry g, const Placement& p, std::uint32_t owner) {
            if (!g.drawable) // bodies, copies and empty meshes
                return;
            place(g, p);
            set.shapes.push_back({std::move(g.kind), owner, object, std::move(g.positions), std::move(g.indices),
                                  std::move(g.surfaces), g.material});
        };
        if (type == 11) { // a template: drawn where type 10 copies place it
            templates[shape->handle] = *shape;
            templateObjects.push_back(shape->handle);
            continue;
        }
        if (type == 10) {
            if (const auto it = templates.find(shape->copyOf); it != templates.end()) {
                Geometry copy = it->second;
                copy.kind = "Placed " + copy.kind;
                emit(std::move(copy), placement, shape->handle);
                std::erase(templateObjects, shape->copyOf);
            }
            continue;
        }
        for (std::size_t k = 0; k < children.size(); ++k)
            emit(std::move(children[k]), childPlacements[k].then(placement), shape->handle);
        if (type != 7)
            emit(std::move(*shape), placement, shape->handle);
    }
    // Templates nothing placed: show them where they are.
    for (const std::uint32_t handle : templateObjects) {
        Geometry g = templates[handle];
        set.shapes.push_back({std::move(g.kind), handle, set.objects, std::move(g.positions), std::move(g.indices),
                              std::move(g.surfaces), g.material});
    }
    return set;
}

namespace {

struct Surface {
    const char* name;
    std::uint32_t color;
};

// Blur.exe's surface table (built by the function at 0x416AD0; ids from 37 on are "Invalid").
constexpr Surface kSurfaces[] = {
    {"Default", 0x9a9a9a},           {"Tarmac", 0x5d6168},           {"Concrete", 0xaaa9a0},
    {"Rock", 0x7d7468},              {"Dirt", 0x8b6b4a},             {"Mud", 0x6b5236},
    {"Grass", 0x5f9a45},             {"Gravel", 0x9c9182},           {"Sand", 0xd8c38f},
    {"WetSand", 0xb39e6e},           {"Water", 0x3f7fbf},            {"Brick", 0xa5533d},
    {"Vegetation", 0x3f7f35},        {"Wood", 0x9c6b3d},             {"Metal", 0x8f9aa6},
    {"Plastic", 0xd0cdd8},           {"Glass", 0x8fc7d9},            {"Rubber", 0x404040},
    {"Upholstery", 0x7f5f8f},        {"Cardboard", 0xb59060},        {"Flesh", 0xe0a080},
    {"TrainTracks", 0x6d5a4a},       {"WaterDeep", 0x23558f},        {"ConcreteFenceChain", 0xb0b0a8},
    {"ConcreteFencePlastic", 0xc0b8a0}, {"MetalHollow", 0x7f8a96},   {"MetalArmco", 0xb7c3cf},
    {"MetalSheet", 0x9aa5b0},        {"MetalFenceChain", 0xa0a8b0},  {"MetalFenceSolid", 0x88939e},
    {"MetalFenceSlatted", 0x96a0aa}, {"MetalSolidPost", 0x6f7a85},   {"DirtLightRough", 0x9a7a55},
    {"DirtHeavyRough", 0x7a5a38},
};

} // namespace

std::string surfaceName(std::uint16_t material)
{
    return material < std::size(kSurfaces) ? kSurfaces[material].name : std::format("Surface {}", material);
}

std::uint32_t surfaceColor(std::uint16_t material) noexcept
{
    return material < std::size(kSurfaces) ? kSurfaces[material].color : 0xc27aa8;
}

void appendCollisionParts(Mesh& mesh, const std::vector<CollisionShape>& shapes, const std::string& group,
                          const std::function<std::string(const CollisionShape&)>& nameOf,
                          const std::function<int(const CollisionShape&)>& nodeOf)
{
    const std::size_t levels = std::max<std::size_t>(1, mesh.lodCount());
    const bool normals = !mesh.normals.empty(), uvs = !mesh.uvs.empty();
    for (const CollisionShape& shape : shapes) {
        // One part per surface material (meshes) or per shape; corners copied
        // per triangle so every face is drawn flat.
        std::map<std::uint16_t, std::vector<std::size_t>> bySurface;
        for (std::size_t t = 0; t < shape.indices.size() / 3; ++t)
            bySurface[t < shape.surfaces.size() ? shape.surfaces[t] : shape.material].push_back(t);
        for (const auto& [surface, triangles] : bySurface) {
            Mesh::Part part;
            part.collision = true;
            part.name = nameOf(shape);
            part.material = std::format("{} · {}", shape.kind, surfaceName(surface));
            part.group = group;
            part.node = nodeOf(shape);
            part.color = surfaceColor(surface);
            if (!shape.surfaces.empty()) // a mesh has many: one part each
                part.name += " · " + surfaceName(surface);
            const Mesh::Range range{static_cast<std::uint32_t>(mesh.indices.size()),
                                    static_cast<std::uint32_t>(triangles.size() * 3)};
            for (const std::size_t t : triangles)
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t v = shape.indices[3 * t + static_cast<std::size_t>(k)];
                    mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertexCount()));
                    mesh.positions.insert(mesh.positions.end(), shape.positions.begin() + 3 * v,
                                          shape.positions.begin() + 3 * v + 3);
                    if (normals)
                        mesh.normals.insert(mesh.normals.end(), {0.0f, 0.0f, 0.0f}); // the viewer works them out
                    if (uvs)
                        mesh.uvs.insert(mesh.uvs.end(), {0.0f, 0.0f});
                }
            part.lods.assign(levels, range); // the game collides with the same shape at every level of detail
            mesh.parts.push_back(std::move(part));
        }
    }
}

// ---- .levelphysics ------------------------------------------------------------------

bool LevelPhysicsDecoder::canDecode(std::string_view extension, ByteSpan) const
{
    return extension == "levelphysics";
}

Result<Mesh> LevelPhysicsDecoder::decode(ByteSpan data) const
{
    auto root = parseChunkFile(data);
    if (!root)
        return std::unexpected(root.error());
    Mesh mesh;
    mesh.leftHanded = true; // Direct3D, like the models
    std::size_t routes = 0, objects = 0;
    std::vector<std::string> problems;
    const auto addStream = [&](const Chunk& chunk, const std::string& group) {
        auto set = readPhysicsStream(data.subspan(chunk.payloadOffset(), chunk.payloadSize()));
        if (!set) {
            problems.push_back(std::format("{}: {}", group, set.error().message));
            return;
        }
        objects += set->objects;
        std::map<std::string, std::size_t> counters;
        std::map<std::size_t, std::string> names; // object -> name, so a mesh split by surface keeps one name
        appendCollisionParts(
            mesh, set->shapes, group,
            [&](const CollisionShape& shape) {
                auto [it, added] = names.try_emplace(shape.object);
                if (added) {
                    std::string kind = shape.kind; // "Triangle mesh 2" reads long in the list: "Mesh 2"
                    if (const auto at = kind.find("Triangle mesh"); at != std::string::npos)
                        kind.replace(at, 13, at ? "mesh" : "Mesh");
                    it->second = std::format("{} {}", kind, ++counters[kind]);
                }
                return it->second;
            },
            [](const CollisionShape&) { return -1; });
    };
    // A race uses one route layout, so only the first is shown at first (the
    // others' barriers would stand across it).
    bool firstRoute = true;
    for (const Chunk& chunk : root->children) {
        if (chunk.name == "Main") {
            addStream(chunk, "Track and walls");
        } else if (chunk.name == "Routes") {
            for (const Chunk& route : chunk.children) {
                if (route.name != "Route")
                    continue;
                ++routes;
                const std::size_t before = mesh.parts.size();
                addStream(route, std::format("Route {}", routes));
                if (mesh.parts.size() == before)
                    continue; // no barriers in this layout
                for (std::size_t p = before; p < mesh.parts.size(); ++p)
                    mesh.parts[p].startHidden = !firstRoute;
                firstRoute = false;
            }
        }
    }
    if (mesh.parts.empty())
        return fail(ErrorCode::InvalidFormat, "no collision shapes found{}{}", problems.empty() ? "" : ": ",
                    problems.empty() ? "" : problems.front());
    mesh.notes.push_back(std::format("Collision: {} object(s), {} part(s) coloured by surface material{}", objects,
                                     mesh.parts.size(),
                                     routes ? std::format(", {} route layout(s) (one shown at first)", routes) : ""));
    for (const std::string& problem : problems)
        mesh.notes.push_back(problem);
    return mesh;
}

} // namespace viewer::games::blur
