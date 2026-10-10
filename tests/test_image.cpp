#include "TestUtil.hpp"

#include "viewer/core/Image.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace viewer;

TEST_CASE("BC1 block decodes endpoint colours")
{
    // c0 = pure red (0xF800), c1 = pure blue (0x001F), every texel index 0 except texel 1 = index 1.
    test::Writer w;
    w.put<std::uint16_t>(0xF800);
    w.put<std::uint16_t>(0x001F);
    w.put<std::uint32_t>(0b0100);
    auto img = image::decodeSurface(w.bytes, image::PixelFormat::BC1, 4, 4);
    REQUIRE(img);
    CHECK(img->rgba[0] == 255);
    CHECK(img->rgba[2] == 0);
    CHECK(img->rgba[4 + 0] == 0);   // texel 1 red
    CHECK(img->rgba[4 + 2] == 255); // texel 1 blue
    CHECK(img->rgba[3] == 255);
}

TEST_CASE("BC1 three-colour mode has transparent index 3")
{
    test::Writer w;
    w.put<std::uint16_t>(0x001F); // c0 <= c1 -> 3-colour mode
    w.put<std::uint16_t>(0xF800);
    w.put<std::uint32_t>(0xFFFFFFFF); // all index 3
    auto img = image::decodeSurface(w.bytes, image::PixelFormat::BC1, 4, 4);
    REQUIRE(img);
    CHECK(img->rgba[3] == 0);
}

TEST_CASE("Uncompressed DDS (A8R8G8B8) is decoded with correct channel order")
{
    test::Writer w;
    w.put("DDS ");
    w.put<std::uint32_t>(124);
    w.put<std::uint32_t>(0x100F);
    w.put<std::uint32_t>(1); // height
    w.put<std::uint32_t>(2); // width
    w.put<std::uint32_t>(8);
    w.put<std::uint32_t>(0);
    w.put<std::uint32_t>(1);
    for (int i = 0; i < 11; ++i)
        w.put<std::uint32_t>(0);
    w.put<std::uint32_t>(32);   // pf size
    w.put<std::uint32_t>(0x41); // RGB | ALPHAPIXELS
    w.put<std::uint32_t>(0);
    w.put<std::uint32_t>(32);
    w.put<std::uint32_t>(0x00FF0000);
    w.put<std::uint32_t>(0x0000FF00);
    w.put<std::uint32_t>(0x000000FF);
    w.put<std::uint32_t>(0xFF000000);
    for (int i = 0; i < 5; ++i)
        w.put<std::uint32_t>(0);
    REQUIRE(w.bytes.size() == 128);
    w.put<std::uint32_t>(0x80112233); // A=80 R=11 G=22 B=33
    w.put<std::uint32_t>(0xFFAABBCC);

    auto img = image::decodeDds(w.bytes);
    REQUIRE(img);
    CHECK(img->width == 2);
    CHECK(img->height == 1);
    CHECK(img->rgba[0] == 0x11);
    CHECK(img->rgba[1] == 0x22);
    CHECK(img->rgba[2] == 0x33);
    CHECK(img->rgba[3] == 0x80);
    CHECK(img->rgba[4] == 0xAA);
}

TEST_CASE("Truncated surfaces fail cleanly")
{
    const ByteBuffer tiny(4);
    CHECK_FALSE(image::decodeSurface(tiny, image::PixelFormat::BC3, 8, 8));
    CHECK_FALSE(image::decodeDds(tiny));
}

TEST_CASE("D3D format codes map to pixel formats")
{
    CHECK(image::fromD3DFormat(21) == image::PixelFormat::A8R8G8B8);
    CHECK(image::fromD3DFormat(28) == image::PixelFormat::A8);
    CHECK(image::fromD3DFormat(0x31545844) == image::PixelFormat::BC1); // "DXT1"
    CHECK_FALSE(image::fromD3DFormat(12345));
}
