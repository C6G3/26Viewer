// Blur levels (levels/<name>/Level.levelrender + Level.levelstream). The
// format needs a whole track to say anything useful, so with BLUR_DIR set
// every shipped level is decoded; without it only the error paths run.

#include "TestUtil.hpp"

#include "ChunkFile.hpp"
#include "Level.hpp"

#include "viewer/core/RandomAccessFile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>

using namespace viewer;
using namespace viewer::games::blur;

TEST_CASE("Chunk names in a fixed 8-byte field (level render files)")
{
    // "LS" written over "NO_ID": junk after the NUL, then size, count, kind.
    test::Writer w;
    w.put(std::string_view("LS\0ID\0\0\0", 8));
    w.put<std::uint32_t>(16);
    w.put<std::uint16_t>(0);
    w.put<std::uint16_t>('A');
    const auto chunk = readChunkHeader(w.bytes, 0);
    REQUIRE(chunk);
    CHECK(chunk->name == "LS");
    CHECK(chunk->headerSize == 16);
    CHECK(chunk->kind == 'A');
}

TEST_CASE("Level decoder rejects files that aren't a level")
{
    CHECK_FALSE(decodeLevel(asBytes("not a level at all"), {}));
    CHECK_FALSE(LevelDecoder().decode(asBytes("anything")));
    // Without access to the folder (a file opened on its own) it says why.
    CHECK_FALSE(LevelDecoder().decodeWith(asBytes("anything"), {}));
    CHECK(LevelDecoder().canDecode("levelrender", {}));
    CHECK(LevelDecoder().canDecode("levelstream", {}));
    CHECK_FALSE(LevelDecoder().canDecode("levelphysics", {}));
}

TEST_CASE("Real Blur levels (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    const auto readAll = [](const std::filesystem::path& path) {
        auto data = readFile(path);
        REQUIRE(data);
        return std::move(*data);
    };
    std::size_t levels = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir) / "levels")) {
        const auto render = entry.path() / "Level.levelrender";
        if (!std::filesystem::exists(render))
            continue;
        INFO(entry.path().filename().string());
        const auto mesh = decodeLevel(readAll(render), readAll(entry.path() / "Level.levelstream"));
        REQUIRE(mesh);
        CHECK(mesh->triangleCount() > 10'000);
        // Every scene node and draw understood.
        for (const std::string& note : mesh->notes) {
            INFO(note);
            CHECK(note.find("skipped") == std::string::npos);
        }
        if (entry.path().filename() == "uk_brighton")
            CHECK(mesh->triangleCount() > 4'500'000);
        ++levels;
    }
    CHECK(levels >= 19);
}
