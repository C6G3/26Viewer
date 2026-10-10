#include "TestUtil.hpp"

#include "ChunkFile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>

using namespace viewer;
using namespace viewer::games::blur;

namespace {

/// A 2x2 A8R8G8B8 Blur texture: Texture{ Header, Data }.
ByteBuffer buildTexture()
{
    test::Writer w;
    w.put("Texture", true); // 8 bytes, already 4-aligned
    w.put<std::uint32_t>(0);   // size, patched below
    w.put<std::uint16_t>(2);
    w.put<std::uint16_t>('C');

    w.put("Header", true);
    w.padTo(4);
    w.put<std::uint32_t>(16 + 8);
    w.put<std::uint16_t>(0);
    w.put<std::uint16_t>('A');
    w.put<std::uint64_t>(0);

    w.put("Data", true);
    w.padTo(4);
    w.put<std::uint32_t>(16 + 28 + 16);
    w.put<std::uint16_t>(0);
    w.put<std::uint16_t>('A');
    for (std::uint32_t v : {2u, 2u, 1u, 1u, 21u, 0u, 3u}) // w, h, depth, mips, D3DFMT_A8R8G8B8, ?, ?
        w.put<std::uint32_t>(v);
    for (std::uint32_t argb : {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0x80FFFFFFu})
        w.put<std::uint32_t>(argb);

    w.patch<std::uint32_t>(8, static_cast<std::uint32_t>(w.bytes.size()));
    return w.bytes;
}

} // namespace

TEST_CASE("Chunk headers: NUL-padded names, size, kind")
{
    const auto bytes = buildTexture();
    auto root = readChunkHeader(bytes, 0);
    REQUIRE(root);
    CHECK(root->name == "Texture");
    CHECK(root->headerSize == 16);
    CHECK(root->size == bytes.size());
    CHECK(root->kind == 'C');

    CHECK_FALSE(readChunkHeader(asBytes("\x01\x02garbage-not-a-chunk"), 0));
}

TEST_CASE("Chunk tree parsing")
{
    auto root = parseChunkFile(buildTexture());
    REQUIRE(root);
    REQUIRE(root->children.size() == 2);
    CHECK(root->children[0].name == "Header");
    CHECK(root->children[1].name == "Data");
    CHECK(root->child("Data")->payloadSize() == 28 + 16);
}

TEST_CASE("Structure parser exposes the chunk tree")
{
    ChunkStructureParser parser;
    const auto bytes = buildTexture();
    REQUIRE(parser.canParse("texture", bytes));
    auto tree = parser.parse(bytes);
    REQUIRE(tree);
    REQUIRE(tree->children.size() == 1);
    CHECK(tree->children[0].label == "Texture");
    CHECK(tree->children[0].children.size() == 2);
}

TEST_CASE("Texture decoder produces RGBA pixels")
{
    TextureDecoder decoder;
    const auto bytes = buildTexture();
    REQUIRE(decoder.canDecode("texture", bytes));
    auto img = decoder.decode(bytes);
    REQUIRE(img);
    CHECK(img->width == 2);
    CHECK(img->height == 2);
    const std::vector<std::uint8_t> expected{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 0x80};
    CHECK(img->rgba == expected);
}

TEST_CASE("Texture rebuild keeps the file and swaps only the pixels")
{
    TextureDecoder decoder;
    const auto bytes = buildTexture();
    auto surfaces = decoder.surfaces(bytes);
    REQUIRE(surfaces);
    CHECK(surfaces->format == image::PixelFormat::A8R8G8B8);
    CHECK(surfaces->mipCount == 1);
    CHECK(surfaces->data.size() == 16);
    auto same = decoder.rebuild(bytes, *surfaces);
    REQUIRE(same);
    CHECK(*same == bytes);

    const auto formats = decoder.importFormats(bytes);
    CHECK(formats.front() == image::PixelFormat::BC1);
    CHECK(std::ranges::contains(formats, image::PixelFormat::A8R8G8B8));

    // An 8x8 DXT5 picture with mips in place of the 2x2 one
    Image picture;
    picture.width = picture.height = 8;
    picture.rgba.assign(8 * 8 * 4, 0x80);
    auto replacement = image::encodeSurfaces(picture, image::PixelFormat::BC3, 4);
    REQUIRE(replacement);
    auto rebuilt = decoder.rebuild(bytes, *replacement);
    REQUIRE(rebuilt);
    auto root = parseChunkFile(*rebuilt);
    REQUIRE(root);
    CHECK(root->size == rebuilt->size());
    REQUIRE(root->child("Header"));
    CHECK(root->child("Data")->payloadSize() == 28 + replacement->data.size());
    auto back = decoder.surfaces(*rebuilt);
    REQUIRE(back);
    CHECK(back->format == image::PixelFormat::BC3);
    CHECK(back->width == 8);
    CHECK(back->mipCount == 4);
    CHECK(back->data == replacement->data);
    auto img = decoder.decode(*rebuilt);
    REQUIRE(img);
    CHECK(std::abs(int(img->rgba[0]) - 0x80) <= 4); // 5:6:5 rounding
    CHECK(img->rgba[3] == 0x80);
}
