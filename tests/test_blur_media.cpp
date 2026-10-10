// Blur audio banks (.baf) and model geometry (.model), on synthetic files built
// here. With BLUR_DIR set, real game files are checked too.

#include "TestUtil.hpp"

#include "BafBank.hpp"
#include "ModelDecoder.hpp"
#include "PakArchive.hpp"

#include "viewer/core/FileTypes.hpp"
#include "viewer/core/Text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>

using namespace viewer;
using namespace viewer::games::blur;

namespace {

// ---- Audio banks -------------------------------------------------------------------------

/// One 33-byte ADPCM frame: header (filter << 4 | shift) then 64 nibbles.
ByteBuffer adpcmFrame(unsigned filter, unsigned shift, std::uint8_t fill)
{
    ByteBuffer f(33, std::byte{fill});
    f[0] = static_cast<std::byte>(filter << 4 | shift);
    return f;
}

void putName(test::Writer& w, std::string_view name, std::size_t size)
{
    std::string padded(name);
    padded.resize(size, '\0');
    w.put(padded);
}

/// BANK with one 4-bit ADPCM sound (type 7) and one PCM16 sound (type 3).
ByteBuffer buildBank(const ByteBuffer& adpcm, std::uint16_t channels, std::uint32_t frames, const ByteBuffer& pcm)
{
    test::Writer w;
    w.put("BANK");
    w.put<std::uint32_t>(0x34);
    w.put<std::uint32_t>(4);
    w.put<std::uint32_t>(2);
    w.put<std::uint16_t>(0);
    putName(w, "TestBank", 32);
    w.bytes.resize(0x34);

    const std::uint32_t dataStart = 0x34 + 76 + 80;
    // type 7
    w.put("WAVE");
    w.put<std::uint32_t>(76);
    w.put<std::uint32_t>(7);
    putName(w, "engine_loop", 32);
    w.put<std::uint32_t>(dataStart);
    w.put<std::uint32_t>(static_cast<std::uint32_t>(adpcm.size()));
    w.put<std::uint32_t>(0x3F800000); // volume 1.0
    w.put<std::uint32_t>(25);
    w.put<std::uint32_t>(0);
    w.put<std::uint32_t>(48000);
    w.put<std::uint32_t>(frames);
    w.put<std::uint16_t>(1); // loop
    w.put<std::uint8_t>(4);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(channels));
    // type 3
    w.put("WAVE");
    w.put<std::uint32_t>(80);
    w.put<std::uint32_t>(3);
    putName(w, "sync", 32);
    w.put<std::uint32_t>(dataStart + static_cast<std::uint32_t>(adpcm.size()));
    w.put<std::uint32_t>(static_cast<std::uint32_t>(pcm.size()));
    w.put<std::uint32_t>(0x3F800000);
    w.put<std::uint32_t>(25);
    w.put<std::uint32_t>(48000);
    w.put<std::uint32_t>(16);
    w.put<std::uint32_t>(2);
    w.put<std::uint32_t>(0);
    w.put<std::uint32_t>(0xFF);
    REQUIRE(w.bytes.size() == dataStart);
    w.put(adpcm);
    w.put(pcm);
    return w.bytes;
}

} // namespace

TEST_CASE("Blur ADPCM decodes nibbles, filters and stereo frames")
{
    SECTION("shift and nibble order")
    {
        // Byte 0x21: low nibble 1 first, then 2. Shift 0 → nibble << 12.
        const auto frame = adpcmFrame(0, 0, 0x21);
        const auto out = baf::decodeAdpcm(frame, 1, 64);
        REQUIRE(out.size() == 64);
        CHECK(out[0] == 4096);
        CHECK(out[1] == 8192);
        // Shift 4 divides by 16; negative nibbles (0xF = -1).
        const auto quiet = baf::decodeAdpcm(adpcmFrame(0, 4, 0xF1), 1, 2);
        CHECK(quiet == std::vector<std::int16_t>{256, -256});
    }
    SECTION("prediction filter 1 (0.9375 × previous sample)")
    {
        ByteBuffer frame = adpcmFrame(1, 0, 0x00);
        frame[1] = std::byte{0x01}; // first sample 4096, then pure prediction
        const auto out = baf::decodeAdpcm(frame, 1, 3);
        CHECK(out == std::vector<std::int16_t>{4096, 3840, 3600});
    }
    SECTION("stereo alternates frames L, R")
    {
        ByteBuffer data = adpcmFrame(0, 0, 0x11); // left: 4096
        const auto right = adpcmFrame(0, 0, 0x22); // right: 8192
        data.insert(data.end(), right.begin(), right.end());
        const auto out = baf::decodeAdpcm(data, 2, 64);
        REQUIRE(out.size() == 128);
        CHECK(out[0] == 4096);
        CHECK(out[1] == 8192);
        CHECK(out[126] == 4096);
        CHECK(out[127] == 8192);
    }
    SECTION("frame count caps the output")
    {
        CHECK(baf::decodeAdpcm(adpcmFrame(0, 0, 0x11), 1, 10).size() == 10);
    }
}

TEST_CASE("Blur audio banks list and decode their sounds")
{
    ByteBuffer adpcm = adpcmFrame(0, 0, 0x11);
    const auto second = adpcmFrame(0, 0, 0x33);
    adpcm.insert(adpcm.end(), second.begin(), second.end());
    const ByteBuffer pcm = test::sampleData(400);
    const ByteBuffer file = buildBank(adpcm, 1, 100, pcm);

    const BafFormat format;
    REQUIRE(format.canOpen("baf", file));
    CHECK_FALSE(format.canOpen("baf", test::sampleData(64)));
    auto bank = format.open(memorySource(file));
    REQUIRE(bank);
    const auto sounds = (*bank)->sounds();
    REQUIRE(sounds.size() == 2);
    CHECK(sounds[0].name == "engine_loop");
    CHECK(sounds[0].channels == 1);
    CHECK(sounds[0].sampleRate == 48000);
    CHECK(sounds[0].frames == 100);
    CHECK(sounds[0].looped);
    CHECK(sounds[1].name == "sync");
    CHECK(sounds[1].channels == 2);
    CHECK(sounds[1].frames == 100); // 400 bytes of 16-bit stereo

    auto clip = (*bank)->decode(0);
    REQUIRE(clip);
    CHECK(clip->frames() == 100);
    CHECK(clip->samples[0] == 4096);
    CHECK(clip->samples[64] == 12288); // second frame: nibble 3
    auto raw = (*bank)->decode(1);
    REQUIRE(raw);
    CHECK(raw->samples.size() == 200);
    CHECK(raw->samples[0] == static_cast<std::int16_t>(loadScalar<std::uint16_t>(pcm, 0)));

    SECTION("truncated banks are rejected")
    {
        ByteBuffer cut = file;
        cut.resize(0x34 + 40);
        CHECK_FALSE(format.open(memorySource(cut)));
    }
}

namespace {

// ---- Models ------------------------------------------------------------------------------

/// Builds the bits of an ARCH scene stream the decoder understands.
struct SceneWriter {
    test::Writer w;
    std::size_t arch = 0;

    SceneWriter()
    {
        w.put("junk"); // ARCH needn't be at offset 0
        arch = w.bytes.size();
        w.put("ARCH");
        w.put<std::uint32_t>(1);
    }
    void blob(ByteSpan data)
    {
        w.put("RA");
        w.put<std::uint16_t>(0);
        w.put<std::uint32_t>(static_cast<std::uint32_t>(data.size()));
        w.put<std::uint32_t>(16);
        // First 16-byte boundary (from ARCH) at least 5 bytes on.
        std::size_t start = w.bytes.size() + 5;
        while ((start - arch) % 16)
            ++start;
        w.bytes.resize(start);
        w.put(data);
    }
    void vertexBuffer(const std::vector<std::array<float, 3>>& positions)
    {
        w.put("RA");
        w.put<std::uint16_t>(0);
        w.put<std::uint32_t>(3); // position, normal, uv
        w.put<std::uint16_t>(0);
        w.put<std::uint16_t>(12);
        w.put<std::uint16_t>(16);
        w.put<std::uint32_t>(static_cast<std::uint32_t>(positions.size()));
        test::Writer data;
        for (const auto& p : positions) {
            for (float f : p) {
                std::uint32_t bits;
                std::memcpy(&bits, &f, 4);
                data.put<std::uint32_t>(bits);
            }
            data.put<std::uint32_t>(0x7F7F7F00); // normal
            data.put<std::uint32_t>(0);          // uv
        }
        blob(data.bytes);
    }
    void indexBuffer(const std::vector<std::uint16_t>& indices)
    {
        w.put("RA");
        w.put<std::uint16_t>(1);
        w.put<std::uint32_t>(static_cast<std::uint32_t>(indices.size()));
        w.put<std::uint32_t>(0);
        test::Writer data;
        for (auto i : indices)
            data.put<std::uint16_t>(i);
        blob(data.bytes);
    }
    struct DrawSpec {
        std::uint32_t start, count, minIndex, vertices;
    };
    /// A group of draws (levels of detail) and its vertex stream (buffer number, byte offset),
    /// optionally followed by unused LOD slots (empty draws, minIndex 0x7FFFFFFF).
    void group(const std::vector<DrawSpec>& lods, std::uint32_t bufferNumber, std::uint32_t byteOffset,
               std::uint32_t emptySlots = 0)
    {
        w.put("RA");
        w.put<std::uint16_t>(0);
        w.put<std::uint32_t>(static_cast<std::uint32_t>(lods.size() + emptySlots));
        for (const auto& d : lods) {
            w.put("RA");
            w.put<std::uint16_t>(0);
            for (auto v : {d.start, d.count, d.minIndex, d.vertices})
                w.put<std::uint32_t>(v);
        }
        for (std::uint32_t i = 0; i < emptySlots; ++i) {
            w.put("RA");
            w.put<std::uint16_t>(0);
            for (auto v : {lods.back().start + lods.back().count, 0u, 0x7FFFFFFFu, 0u})
                w.put<std::uint32_t>(v);
        }
        w.put("RA");
        w.put<std::uint16_t>(0);
        w.put<std::uint32_t>(1); // one stream
        w.put("RA");
        w.put<std::uint16_t>(0);
        for (auto v : {bufferNumber, 0u, byteOffset, 0u})
            w.put<std::uint32_t>(v);
        w.put("RA");
        w.put<std::uint16_t>(0);
        w.put<std::uint32_t>(0x12345678); // whatever comes next
    }
};

} // namespace

TEST_CASE("Blur models: vertex/index buffers and a stitched strip")
{
    SceneWriter s;
    s.vertexBuffer({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
    s.indexBuffer({0, 1, 2, 3}); // strip: two triangles
    s.group({{0, 4, 0, 4}}, 0, 0);

    const ModelDecoder decoder;
    REQUIRE(decoder.canDecode("model", s.w.bytes));
    auto mesh = decoder.decode(s.w.bytes);
    REQUIRE(mesh);
    CHECK(mesh->vertexCount() == 4);
    CHECK(mesh->positions[3] == 1.0f); // vertex 1 x
    REQUIRE(mesh->triangleCount() == 2);
    // Strip winding alternates: (0,1,2) then (1,3,2).
    CHECK(std::vector<std::uint32_t>(mesh->indices.begin(), mesh->indices.end())
          == std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2});
}

TEST_CASE("Blur models: sub-meshes share a buffer via stream offsets; each keeps its LODs")
{
    SceneWriter s;
    // One buffer with two 3-vertex sub-meshes; each group indexes from 0.
    s.vertexBuffer({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {5, 5, 5}, {6, 5, 5}, {5, 6, 5}});
    // Indices: part A (list), part A low LOD, part B (list).
    s.indexBuffer({0, 1, 2, 0, 1, 2, 0, 1, 2});
    s.group({{0, 3, 0, 3}, {3, 3, 0, 3}}, 7, 0);   // two LODs of part A
    s.group({{6, 3, 0, 3}}, 7, 3 * 20);             // part B starts at vertex 3

    auto mesh = ModelDecoder().decode(s.w.bytes);
    REQUIRE(mesh);
    REQUIRE(mesh->parts.size() == 2);
    CHECK(mesh->lodCount() == 2);
    CHECK(mesh->triangleCount(0) == 2); // LOD 0: part A + part B
    CHECK(mesh->triangleCount(1) == 1); // LOD 1: only part A has one
    const Mesh::Part& b = mesh->parts[1];
    REQUIRE(b.lods.size() == 1);
    CHECK(mesh->indices[b.lods[0].firstIndex] == 3); // part B → vertices 3..5
}

TEST_CASE("Blur models: empty LOD slots don't hide the stream")
{
    SceneWriter s;
    s.vertexBuffer({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {5, 5, 5}, {6, 5, 5}, {5, 6, 5}});
    s.indexBuffer({0, 1, 2, 0, 1, 2});
    s.group({{0, 3, 0, 3}}, 7, 0);
    s.group({{3, 3, 0, 3}}, 7, 3 * 20, 2); // part B, then two unused LOD slots

    auto mesh = ModelDecoder().decode(s.w.bytes);
    REQUIRE(mesh);
    REQUIRE(mesh->triangleCount() == 2);
    const Mesh::Part& b = mesh->parts[1];
    CHECK(mesh->indices[b.lods[0].firstIndex] == 3); // part B still starts at vertex 3
    REQUIRE(b.lods.size() == 3);                     // its two empty slots are levels it isn't drawn at
    CHECK(b.lods[1].indexCount == 0);
    CHECK(mesh->lodCount() == 3);
}

TEST_CASE("Blur models without geometry fail cleanly")
{
    const ModelDecoder decoder;
    CHECK_FALSE(decoder.decode(test::sampleData(500)));
    SceneWriter s; // ARCH but no buffers
    CHECK_FALSE(decoder.decode(s.w.bytes));
}

TEST_CASE("Real Blur models and audio banks (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "root.pak");
    REQUIRE(pak);
    const auto find = [&](std::string_view suffix) -> std::optional<std::size_t> {
        const auto entries = (*pak)->entries();
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].path.ends_with(suffix))
                return i;
        return std::nullopt;
    };

    const auto chair = find("uk_brighton/objects/cheapchair.model");
    REQUIRE(chair);
    auto mesh = ModelDecoder().decode((*pak)->read(*chair).value());
    REQUIRE(mesh);
    CHECK(mesh->vertexCount() == 1147);
    CHECK(mesh->triangleCount() == 1004);

    const auto engine = find("audio/toyota_i6.baf");
    REQUIRE(engine);
    auto bank = BafFormat().open((*pak)->openEntry(*engine).value());
    REQUIRE(bank);
    REQUIRE((*bank)->sounds().size() == 26);
    for (std::size_t i = 0; i < (*bank)->sounds().size(); ++i) {
        auto clip = (*bank)->decode(i);
        REQUIRE(clip);
        CHECK(clip->frames() == (*bank)->sounds()[i].frames);
    }
}

TEST_CASE("Every real Blur text file survives load + save byte for byte (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    std::size_t checked = 0;
    for (const char* name : {"gamedata.pak", "root.pak"}) {
        auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / name);
        REQUIRE(pak);
        const auto entries = (*pak)->entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const std::string ext = extensionOf(entries[i].path);
            if (ext != "csv" && ext != "txt" && ext != "wbl")
                continue;
            const ByteBuffer original = (*pak)->read(i).value();
            auto doc = decodeText(original);
            REQUIRE(doc);
            auto saved = encodeText(*doc);
            REQUIRE(saved);
            INFO(entries[i].path);
            CHECK(*saved == original);
            ++checked;
        }
    }
    CHECK(checked > 400); // 480 event tables, the UTF-16 .txt files and the bank list
}
