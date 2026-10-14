// Blur collision (physics streams: a model's "Coll" chunk, .levelphysics), on
// synthetic streams built here. With BLUR_DIR set, real game files are checked too.

#include "TestUtil.hpp"

#include "ChunkFile.hpp"
#include "ModelDecoder.hpp"
#include "PakArchive.hpp"
#include "Physics.hpp"

#include "viewer/core/RandomAccessFile.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <format>

using namespace viewer;
using namespace viewer::games::blur;
using Catch::Approx;

namespace {

constexpr std::uint32_t kEnd = 0x0FFFFFFF;

struct Stream : test::Writer {
    std::uint32_t version = 10;

    void f(float v) { put(std::bit_cast<std::uint32_t>(v)); }
    void f3(float x, float y, float z)
    {
        f(x);
        f(y);
        f(z);
    }
    /// The 13 words every object starts with (12 before version 7).
    void base(std::uint32_t handle, std::uint32_t material = 0, float ox = 0, float oy = 0, float oz = 0)
    {
        f(0.5f);
        f(0.01f);
        put<std::uint32_t>(0);
        if (version >= 2)
            put(material);
        put<std::uint32_t>(351); // flags
        put<std::uint32_t>(1);
        f(1.0f);
        put(handle);
        f3(ox, oy, oz);
        put<std::uint32_t>(1);
        if (version >= 7)
            put<std::uint32_t>(0);
    }
    void identity()
    {
        f3(1, 0, 0);
        f3(0, 1, 0);
        f3(0, 0, 1);
    }
    /// An object's placement: position, rotation, inertia.
    void placement(float x, float y, float z)
    {
        f3(x, y, z);
        identity();
        identity();
    }
    /// A shape's place in its body: f32, rotation, position, f32[9].
    void shapePlacement(float x, float y, float z)
    {
        f(0.04f);
        identity();
        f3(x, y, z);
        identity();
    }
    void begin()
    {
        put("Head");
        put(version);
    }
    /// A one-triangle mesh in one tree leaf, its corners on surface `surface`.
    void triangleMesh(std::uint16_t surface)
    {
        put<std::uint32_t>(3); // vertices
        put<std::uint32_t>(0); // extra words
        put<std::uint32_t>(16);
        put<std::uint32_t>(1); // nodes
        f3(0, 0, 0);
        f3(1, 1, 0);
        f3(0, 0, 0);
        f3(1, 0, 0);
        f3(0, 1, 0);
        for (int i = 0; i < 6; ++i) // quantised box
            put<std::uint16_t>(0);
        put<std::uint32_t>(1); // triangles
        put<std::uint32_t>(0); // where they sit in memory
        put<std::uint32_t>(0); // first vertex
        put<std::uint32_t>(3); // vertex count
        // a group: 15 bytes (surface at +6, count at +14), then the triangle
        put<std::uint16_t>(0x17f);
        put<std::uint16_t>(0);
        put<std::uint16_t>(1);
        put(surface);
        for (int i = 0; i < 6; ++i)
            put<std::uint8_t>(0);
        put<std::uint8_t>(1);
        put<std::uint8_t>(0);
        put<std::uint8_t>(1);
        put<std::uint8_t>(2);
        put<std::uint32_t>(0x1FF800); // edge flags
        put<std::uint32_t>(0);
        if (version >= 7)
            put<std::uint32_t>(0); // no extra records
    }
};

const CollisionShape* firstOf(const CollisionSet& set, std::string_view kind)
{
    const auto it = std::ranges::find_if(set.shapes, [&](const CollisionShape& s) { return s.kind == kind; });
    return it == set.shapes.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("Blur physics: boxes, bodies with hulls, placed mesh copies")
{
    Stream s;
    s.begin();
    // A 2x4x6 concrete box at x = 10.
    s.put<std::uint32_t>(2);
    s.base(0x20, 2);
    s.f3(1, 2, 3);
    s.placement(10, 0, 0);
    // A car part: a body (tag 0xC0000006, centre of mass (1, 0, 0)) at z = 7
    // holding a hull placed at y = 5, its points stored around the body's centre
    // of mass (the hull's own offset doesn't move it).
    s.put<std::uint32_t>(7);
    s.base(0xC0000006, 0, 1, 0, 0);
    s.put<std::uint32_t>(1); // shapes
    s.put<std::uint32_t>(0xE0000000);
    s.put<std::uint32_t>(3);
    s.base(0xE0000000, 15, 0.3f, 0, 0);
    s.put<std::uint32_t>(4);
    s.put<std::uint32_t>(4);
    s.f3(0, 0, 0);
    s.f3(1, 0, 0);
    s.f3(0, 1, 0);
    s.f3(0, 0, 1);
    for (std::uint32_t t : {0u, 1u, 2u, 0u, 1u, 3u, 0u, 2u, 3u, 1u, 2u, 3u})
        s.put(t);
    s.shapePlacement(0, 5, 0);
    s.placement(0, 0, 7);
    // A template mesh (type 11, handle 0x80000000) and a copy of it at y = 100.
    s.put<std::uint32_t>(11);
    s.base(0x80000000);
    s.triangleMesh(26);
    s.placement(0, 0, 0);
    s.put<std::uint32_t>(10);
    s.base(0xA0000000);
    s.put<std::uint32_t>(0x80000000);
    s.placement(0, 100, 0);
    s.put(kEnd);
    s.put<std::uint32_t>(0xFFFFFFFF);

    auto set = readPhysicsStream(s.bytes);
    REQUIRE(set);
    CHECK(set->objects == 4);
    REQUIRE(set->shapes.size() == 3); // the template is only drawn where it's placed

    const CollisionShape* box = firstOf(*set, "Box");
    REQUIRE(box);
    CHECK(box->positions.size() == 8 * 3);
    CHECK(box->indices.size() == 12 * 3);
    CHECK(box->material == 2);
    CHECK(*std::ranges::min_element(box->positions) == Approx(-3.0f));
    CHECK(box->positions[0] == Approx(9.0f)); // x = -1 moved to 10
    CHECK(box->owner == 0x20);

    const CollisionShape* hull = firstOf(*set, "Convex hull");
    REQUIRE(hull);
    CHECK(hull->owner == 0xC0000006);
    CHECK(hull->material == 15);
    CHECK(hull->positions[0] == Approx(1.0f)); // (0,0,0) + body's centre (1,0,0) + shape (0,5,0) + body (0,0,7)
    CHECK(hull->positions[1] == Approx(5.0f));
    CHECK(hull->positions[2] == Approx(7.0f));
    CHECK(hull->indices.size() == 12);

    const CollisionShape* copy = firstOf(*set, "Placed Triangle mesh");
    REQUIRE(copy);
    CHECK(copy->indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(copy->surfaces == std::vector<std::uint16_t>{26});
    CHECK(copy->positions[1] == Approx(100.0f));
    CHECK(surfaceName(26) == "MetalArmco");
    CHECK(surfaceName(1) == "Tarmac");
}

TEST_CASE("Blur physics: version 6 streams (no extra base word, no mesh records)")
{
    Stream s;
    s.version = 6;
    s.begin();
    s.put<std::uint32_t>(7);
    s.base(0xC0000001);
    s.put<std::uint32_t>(2);
    s.put<std::uint32_t>(0xE0000000);
    s.put<std::uint32_t>(6); // cylinder 1.2 high, radius 0.6
    s.base(0xE0000000, 14);
    s.f(1.2f);
    s.f(0.6f);
    s.shapePlacement(0, 0, 0);
    s.put<std::uint32_t>(0); // an empty mesh
    s.base(0);
    for (int i = 0; i < 10; ++i)
        s.put<std::uint32_t>(0);
    s.shapePlacement(0, 0, 0);
    s.placement(0, 0, 0);
    s.put(kEnd);

    auto set = readPhysicsStream(s.bytes);
    REQUIRE(set);
    REQUIRE(set->shapes.size() == 1);
    CHECK(set->shapes[0].kind == "Cylinder");
    CHECK(set->shapes[0].material == 14);
    CHECK(*std::ranges::max_element(set->shapes[0].positions) == Approx(0.6f));
}

TEST_CASE("Blur physics: damaged streams fail cleanly")
{
    Stream s;
    s.begin();
    s.put<std::uint32_t>(3);
    s.base(0);
    s.put<std::uint32_t>(4);
    s.put<std::uint32_t>(4000000); // far more triangles than there are bytes
    CHECK_FALSE(readPhysicsStream(s.bytes));
    CHECK_FALSE(readPhysicsStream(ByteBuffer(16)));

    Stream unknown;
    unknown.begin();
    unknown.put<std::uint32_t>(99);
    unknown.base(0);
    CHECK_FALSE(readPhysicsStream(unknown.bytes));
}

TEST_CASE("Blur chunk files: a root container stored 4 bytes short keeps its last child")
{
    test::Writer w;
    w.put("Root", true);
    w.padTo(4);
    w.put<std::uint32_t>(0); // size, patched below (4 short)
    w.put<std::uint16_t>(2);
    w.put<std::uint16_t>('C');
    for (const char* name : {"Head", "Coll"}) {
        w.put(name, true);
        w.padTo(4);
        w.put<std::uint32_t>(24);
        w.put<std::uint16_t>(0);
        w.put<std::uint16_t>('A');
        w.put<std::uint64_t>(0x1122334455667788);
    }
    w.patch<std::uint32_t>(8, static_cast<std::uint32_t>(w.bytes.size() - 4));
    auto root = parseChunkFile(w.bytes);
    REQUIRE(root);
    CHECK(root->children.size() == 2);
    CHECK(root->child("Coll"));
}

TEST_CASE("Real Blur collision (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "root.pak");
    REQUIRE(pak);
    const auto entries = (*pak)->entries();

    // The Audi: a hull per moving part, named after its element, plus the body colliders.
    const auto audi = std::ranges::find_if(entries, [](const auto& e) {
        return e.path.ends_with("models/vehicles/audi_s3_stock.model");
    });
    REQUIRE(audi != entries.end());
    auto car = ModelDecoder().decode((*pak)->read(static_cast<std::size_t>(audi - entries.begin())).value());
    REQUIRE(car);
    std::vector<const Mesh::Part*> collision;
    for (const Mesh::Part& p : car->parts)
        if (p.collision)
            collision.push_back(&p);
    CHECK(collision.size() == 19);
    const auto named = [&](std::string_view name) {
        const auto it = std::ranges::find_if(collision, [&](const Mesh::Part* p) { return p->name == name; });
        return it == collision.end() ? nullptr : *it;
    };
    REQUIRE(named("E_CarCollider"));
    CHECK(car->isHelper(*named("E_CarCollider")));
    CHECK(collision.front()->lods.size() == car->lodCount()); // drawn at every detail level
    // The back-left tyre's hull sits on the rendered tyre: centre x -0.774 (wheel -0.818, hull centre +0.044).
    const Mesh::Part* tyre = named("e_wheel_backleft");
    REQUIRE(tyre);
    float lo = 1e9f, hi = -1e9f;
    for (std::uint32_t i = tyre->lods[0].firstIndex; i < tyre->lods[0].firstIndex + tyre->lods[0].indexCount; ++i) {
        lo = std::min(lo, car->positions[3 * car->indices[i]]);
        hi = std::max(hi, car->positions[3 * car->indices[i]]);
    }
    CHECK((lo + hi) / 2 == Approx(-0.774f).margin(0.01f));

    // Hinged panels' hulls sit on their panels (stored around their body's centre
    // of mass): the ratrod beetle's bonnet, door and wing, and the Defender's
    // boot, whose hull is turned inside its body.
    const auto centreOf = [](const Mesh& mesh, std::string_view name, bool collisionPart) {
        std::array<float, 3> lo3{1e9f, 1e9f, 1e9f}, hi3{-1e9f, -1e9f, -1e9f};
        for (const Mesh::Part& p : mesh.parts) {
            if (p.collision != collisionPart || p.lods.empty() || p.node < 0
                || mesh.nodes[static_cast<std::size_t>(p.node)].name != name)
                continue;
            for (std::uint32_t i = p.lods[0].firstIndex; i < p.lods[0].firstIndex + p.lods[0].indexCount; ++i)
                for (std::size_t k = 0; k < 3; ++k) {
                    lo3[k] = std::min(lo3[k], mesh.positions[3 * mesh.indices[i] + k]);
                    hi3[k] = std::max(hi3[k], mesh.positions[3 * mesh.indices[i] + k]);
                }
        }
        return std::array<float, 3>{(lo3[0] + hi3[0]) / 2, (lo3[1] + hi3[1]) / 2, (lo3[2] + hi3[2]) / 2};
    };
    const auto load = [&](std::string_view suffix) {
        const auto it = std::ranges::find_if(entries, [&](const auto& e) { return e.path.ends_with(suffix); });
        REQUIRE(it != entries.end());
        auto mesh = ModelDecoder().decode((*pak)->read(static_cast<std::size_t>(it - entries.begin())).value());
        REQUIRE(mesh);
        return std::move(*mesh);
    };
    const Mesh beetle = load("models/vehicles/volkswagen_beetle_rat.model");
    const Mesh defender = load("models/vehicles/landrover_defendersvx_stock.model");
    const std::pair<const Mesh*, const char*> panels[] = {
        {&beetle, "e_bonnet"}, {&beetle, "e_door_left"}, {&beetle, "e_wing_frontright"}, {&defender, "e_boot"}};
    for (const auto& [mesh, name] : panels) {
        INFO(name);
        const auto hull = centreOf(*mesh, name, true), panel = centreOf(*mesh, name, false);
        for (std::size_t k = 0; k < 3; ++k)
            CHECK(hull[k] == Approx(panel[k]).margin(0.12f));
    }

    // Every model's collision reads to the end.
    std::size_t streams = 0;
    std::vector<std::string> failures;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].path.ends_with(".model") && !entries[i].path.ends_with(".particlemesh"))
            continue;
        const ByteBuffer data = (*pak)->read(i).value();
        auto root = parseChunkFile(data);
        const Chunk* coll = root ? root->child("Coll") : nullptr;
        if (!coll)
            continue;
        ++streams;
        if (auto set = readPhysicsStream(ByteSpan(data).subspan(coll->payloadOffset(), coll->payloadSize())); !set)
            failures.push_back(std::format("{}: {}", entries[i].path, set.error().message));
    }
    INFO((failures.empty() ? std::string() : failures.front()));
    CHECK(streams > 1500);
    CHECK(failures.empty());

    // Every level: the track and route barriers, by surface.
    std::size_t levels = 0;
    for (const auto& folder : std::filesystem::directory_iterator(std::filesystem::path(dir) / "levels")) {
        for (const auto& file : std::filesystem::directory_iterator(folder.path())) {
            if (file.path().extension() != ".levelphysics")
                continue;
            ++levels;
            INFO(file.path().string());
            const auto data = readFile(file.path());
            REQUIRE(data);
            auto mesh = LevelPhysicsDecoder().decode(*data);
            REQUIRE(mesh);
            CHECK(mesh->triangleCount() > 2000);
            CHECK(std::ranges::none_of(mesh->notes, [](const std::string& n) { return n.contains(':') && !n.starts_with("Collision"); }));
            if (folder.path().filename() == "uk_brighton") {
                const auto tarmac = std::ranges::find(mesh->parts, std::string("Mesh 1 · Tarmac"), &Mesh::Part::name);
                REQUIRE(tarmac != mesh->parts.end());
                CHECK(tarmac->lods[0].indexCount == 27418 * 3);
                CHECK(std::ranges::count_if(mesh->parts, [](const Mesh::Part& p) { return p.startHidden; }) > 0);
            }
        }
    }
    CHECK(levels == 19);
}
