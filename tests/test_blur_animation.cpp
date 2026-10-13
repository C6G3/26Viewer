// Skeletons, animation cycles and skinned models. The pose maths is checked on
// a tiny skeleton built here; with BLUR_DIR set, real game files are checked too.

#include "Animation.hpp"
#include "ModelDecoder.hpp"
#include "PakArchive.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

using namespace viewer;
using namespace viewer::games::blur;

namespace {

bool nearIdentity(const Matrix43& m, float tolerance = 1e-3f)
{
    static constexpr Matrix43 identity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    for (std::size_t i = 0; i < 12; ++i)
        if (std::abs(m[i] - identity[i]) > tolerance)
            return false;
    return true;
}

} // namespace

TEST_CASE("Poses follow the bone hierarchy and interpolate keys")
{
    // Root at the origin, a child one unit up the y axis.
    Skeleton skeleton;
    Skeleton::Bone root;
    root.name = "root";
    skeleton.bones.push_back(root);
    Skeleton::Bone child;
    child.name = "child";
    child.parent = 0;
    child.rest.position = {0, 1, 0};
    child.inverseBind = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0};
    skeleton.bones.push_back(child);
    CHECK(skeleton.find("CHILD") == 1);

    // At rest, skinning changes nothing.
    const auto rest = anim::skinMatrices(skeleton, anim::worldMatrices(skeleton, anim::sample(skeleton, nullptr, 0)));
    CHECK(nearIdentity(rest[0]));
    CHECK(nearIdentity(rest[1]));

    // The root turns 90 degrees about z over 10 frames: halfway it is at 45.
    Animation spin;
    spin.frameCount = 11;
    const float h = std::sqrt(0.5f);
    spin.channels.push_back({"root", Animation::Kind::Rotation, {0, 10}, {{0, 0, 0, 1}, {0, 0, h, h}}});
    CHECK(anim::fits(skeleton, spin));
    const auto world = anim::worldMatrices(skeleton, anim::sample(skeleton, &spin, 5));
    // The child's origin (0, 1, 0) turned 45 degrees about z (row vectors: x' = x cos - y sin).
    const auto p = anim::apply(world[1], {0, 0, 0});
    CHECK(p[0] == Catch::Approx(-std::sin(0.785398f)).margin(1e-4));
    CHECK(p[1] == Catch::Approx(std::cos(0.785398f)).margin(1e-4));
}

TEST_CASE("Real Blur skeletons, cycles and skinned characters (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "root.pak");
    REQUIRE(pak);
    const auto entries = (*pak)->entries();
    const auto read = [&](std::string_view suffix) {
        const auto it = std::ranges::find_if(entries, [&](const auto& e) { return e.path.ends_with(suffix); });
        REQUIRE(it != entries.end());
        return (*pak)->read(static_cast<std::size_t>(it - entries.begin())).value();
    };

    // The driver skeleton: 32 bones, hips at 0.87 m, its rest pose matching its bind pose.
    const auto skeletonBytes = read("animations/igm_driver_ml.skeleton");
    REQUIRE(SkeletonFileDecoder().canDecode("skeleton", skeletonBytes));
    const auto skeleton = SkeletonFileDecoder().decode(skeletonBytes);
    REQUIRE(skeleton);
    REQUIRE(skeleton->bones.size() == 32);
    CHECK(skeleton->bones[1].name == "hips");
    CHECK(skeleton->bones[1].rest.position[1] == Catch::Approx(0.873f).margin(1e-3));
    for (const Matrix43& m : anim::skinMatrices(*skeleton, anim::worldMatrices(*skeleton, anim::sample(*skeleton, nullptr, 0))))
        CHECK(nearIdentity(m));

    // The player model is skinned: every skinned part names bones of that skeleton.
    const auto player = ModelDecoder().decode(read("characters/male/player/igm_player_ml.model"));
    REQUIRE(player);
    CHECK(player->boneSlots.size() == 4 * player->vertexCount());
    std::size_t skinned = 0;
    for (const Mesh::Part& part : player->parts) {
        if (part.bones.empty())
            continue;
        ++skinned;
        for (const std::string& bone : part.bones)
            CHECK(skeleton->find(bone) >= 0);
    }
    CHECK(skinned > 10);

    // The seated base pose: every channel constant, all for that skeleton.
    const auto base = CycleDecoder().decode(read("animations/drivers/neutral/neutral_base.cycle"));
    REQUIRE(base);
    CHECK(base->channels.size() == 36);
    CHECK(base->notes.empty());
    CHECK(anim::fits(*skeleton, *base));

    // A prop's still parts keep their rest rotation: the helicopter's blade planes
    // are turned the same way in its cycle as in its skeleton.
    const auto copter = SkeletonFileDecoder().decode(read("animations/specialfx/helicopter1.skeleton"));
    const auto copterCycle = CycleDecoder().decode(read("animations/specialfx/helicopter1.cycle"));
    REQUIRE(copter);
    REQUIRE(copterCycle);
    std::size_t compared = 0;
    for (const Animation::Channel& ch : copterCycle->channels) {
        const int bone = copter->find(ch.bone);
        if (ch.kind != Animation::Kind::Rotation || ch.values.size() != 1 || bone < 0)
            continue;
        const auto& rest = copter->bones[static_cast<std::size_t>(bone)].rest.rotation;
        const auto& q = ch.values.front();
        CHECK(std::abs(q[0] * rest[0] + q[1] * rest[1] + q[2] * rest[2] + q[3] * rest[3]) == Catch::Approx(1.0f).margin(1e-4));
        ++compared;
    }
    CHECK(compared >= 9);

    // Its main rotor spins at a steady 72 degrees per key (every 3 frames), across all three blocks.
    const auto rotor = std::ranges::find_if(copterCycle->channels, [](const Animation::Channel& ch) {
        return ch.kind == Animation::Kind::Rotation && ch.values.size() > 100;
    });
    REQUIRE(rotor != copterCycle->channels.end());
    CHECK(rotor->frames.back() == Catch::Approx(599.0f));
    for (std::size_t k = 1; k + 1 < rotor->values.size(); ++k) {
        const auto& a = rotor->values[k - 1];
        const auto& c = rotor->values[k];
        const float dot = std::abs(a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3]);
        CHECK(2.0f * std::acos(std::min(dot, 1.0f)) * 57.29578f == Catch::Approx(72.0f).margin(1.5f));
    }

    // The steering animation moves 14 of the driver's bones.
    const auto steering = CycleDecoder().decode(read("animations/drivers/neutral/neutral_steering.cycle"));
    REQUIRE(steering);
    CHECK(std::ranges::count_if(steering->channels, [](const Animation::Channel& ch) { return ch.values.size() > 2; }) == 14);

    // Every skeleton and cycle in the game reads.
    std::size_t skeletons = 0, cycles = 0, failed = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& path = entries[i].path;
        const bool isSkeleton = path.ends_with(".skeleton"), isCycle = path.ends_with(".cycle");
        if (!isSkeleton && !isCycle)
            continue;
        const auto bytes = (*pak)->read(i).value();
        if (isSkeleton && SkeletonFileDecoder().canDecode("skeleton", bytes)) {
            ++skeletons;
            failed += !SkeletonFileDecoder().decode(bytes);
        } else if (isCycle) {
            ++cycles;
            if (const auto c = CycleDecoder().decode(bytes); !c) {
                ++failed;
                WARN(path << ": " << c.error().message);
            }
        }
    }
    CHECK(skeletons >= 18);
    CHECK(cycles > 700);
    CHECK(failed == 0);
}
