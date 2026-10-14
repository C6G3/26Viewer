#include "TestUtil.hpp"

#include "viewer/core/Image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

namespace {

/// A smooth test picture: colour gradients, an alpha ramp and a few hard edges.
Image testPicture(std::uint32_t w, std::uint32_t h)
{
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = img.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<std::uint8_t>((x / 16 + y / 16) % 2 ? 200 : 40);
            p[3] = static_cast<std::uint8_t>((x + y) * 255 / (w + h - 2));
        }
    }
    return img;
}

/// Peak signal-to-noise ratio of the colour channels (higher is closer; 40 dB+
/// looks identical). With `skipSeeThrough`, pixels under 50% alpha don't count
/// (DXT1 stores those as transparent black).
double psnr(const Image& a, const Image& b, bool skipSeeThrough = false)
{
    double sum = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        if (i % 4 == 3 || (skipSeeThrough && a.rgba[i - i % 4 + 3] < 128))
            continue;
        const double d = double(a.rgba[i]) - double(b.rgba[i]);
        sum += d * d;
    }
    const double mse = sum / (a.rgba.size() / 4 * 3);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

TEST_CASE("DXT encoders reproduce pictures closely")
{
    const Image src = testPicture(64, 64);
    for (const auto format : {image::PixelFormat::BC1, image::PixelFormat::BC2, image::PixelFormat::BC3}) {
        INFO(image::pixelFormatName(format));
        auto bytes = image::encodeSurface(src, format);
        REQUIRE(bytes);
        CHECK(bytes->size() == image::surfaceSize(format, 64, 64));
        auto back = image::decodeSurface(*bytes, format, 64, 64);
        REQUIRE(back);
        CHECK(psnr(src, *back, format == image::PixelFormat::BC1) > 35.0);
        int worstAlpha = 0;
        for (std::size_t i = 3; i < src.rgba.size(); i += 4) {
            const int expected = format == image::PixelFormat::BC1 ? (src.rgba[i] < 128 ? 0 : 255) : src.rgba[i];
            worstAlpha = std::max(worstAlpha, std::abs(expected - int(back->rgba[i])));
        }
        CHECK(worstAlpha <= (format == image::PixelFormat::BC2 ? 9 : format == image::PixelFormat::BC3 ? 4 : 0));
    }
}

TEST_CASE("A flat colour survives DXT1 within 5:6:5 rounding")
{
    Image img;
    img.width = img.height = 4;
    for (int i = 0; i < 16; ++i)
        img.rgba.insert(img.rgba.end(), {200, 100, 50, 255});
    auto bytes = image::encodeSurface(img, image::PixelFormat::BC1);
    REQUIRE(bytes);
    auto back = image::decodeSurface(*bytes, image::PixelFormat::BC1, 4, 4);
    REQUIRE(back);
    for (std::size_t i = 0; i < 16 * 4; ++i)
        CHECK(std::abs(int(back->rgba[i]) - int(img.rgba[i])) <= 4);
}

TEST_CASE("Uncompressed encoders round-trip exactly")
{
    const Image src = testPicture(8, 8);
    for (const auto format : {image::PixelFormat::A8R8G8B8, image::PixelFormat::A8B8G8R8}) {
        auto bytes = image::encodeSurface(src, format);
        REQUIRE(bytes);
        auto back = image::decodeSurface(*bytes, format, 8, 8);
        REQUIRE(back);
        CHECK(back->rgba == src.rgba);
    }
    // A8 keeps the alpha channel
    auto a8 = image::encodeSurface(src, image::PixelFormat::A8);
    REQUIRE(a8);
    for (std::size_t i = 0; i < 64; ++i)
        CHECK(std::to_integer<std::uint8_t>((*a8)[i]) == src.rgba[i * 4 + 3]);
}

TEST_CASE("Mip chains: sizes, levels and box filtering")
{
    CHECK(image::fullMipCount(256, 256) == 9);
    CHECK(image::fullMipCount(256, 64) == 9);
    CHECK(image::fullMipCount(1, 1) == 1);
    CHECK(image::chainSize(image::PixelFormat::BC1, 8, 8, 4) == 32 + 8 + 8 + 8);

    Image img;
    img.width = 2;
    img.height = 2;
    img.rgba = {0, 0, 0, 255, 100, 100, 100, 255, 200, 200, 200, 255, 100, 100, 100, 255};
    const Image half = image::halfSize(img);
    CHECK(half.width == 1);
    CHECK(half.height == 1);
    CHECK(half.rgba == std::vector<std::uint8_t>{100, 100, 100, 255});
}

TEST_CASE("DDS files written with mips read back unchanged")
{
    auto s = image::encodeSurfaces(testPicture(32, 16), image::PixelFormat::BC3, 99);
    REQUIRE(s);
    CHECK(s->mipCount == 6);
    CHECK(s->data.size() == image::chainSize(image::PixelFormat::BC3, 32, 16, 6));
    for (const auto format : {image::PixelFormat::BC3, image::PixelFormat::A8R8G8B8, image::PixelFormat::A8}) {
        INFO(image::pixelFormatName(format));
        auto surfaces = image::encodeSurfaces(testPicture(32, 16), format, 6);
        REQUIRE(surfaces);
        auto dds = image::writeDds(*surfaces);
        REQUIRE(dds);
        auto back = image::readDds(*dds);
        REQUIRE(back);
        CHECK(back->format == format);
        CHECK(back->width == 32);
        CHECK(back->height == 16);
        CHECK(back->mipCount == 6);
        CHECK(back->data == surfaces->data);
        CHECK(image::decodeDds(*dds));
    }
    CHECK(image::toD3DFormat(image::PixelFormat::BC1) == 0x31545844);
    CHECK(image::fromD3DFormat(image::toD3DFormat(image::PixelFormat::A8L8)) == image::PixelFormat::A8L8);
}

TEST_CASE("Each stored mip level and layer decodes on its own")
{
    // Two layers of an uncompressed 8x4 chain: level m of layer l is filled with 10 * l + m.
    image::Surfaces s;
    s.format = image::PixelFormat::A8;
    s.width = 8;
    s.height = 4;
    s.mipCount = 4;
    s.layers = 2;
    for (std::uint8_t l = 0; l < 2; ++l)
        for (std::uint32_t m = 0; m < 4; ++m)
            s.data.insert(s.data.end(), image::surfaceSize(s.format, std::max(1u, 8u >> m), std::max(1u, 4u >> m)),
                          static_cast<std::byte>(10 * l + m));
    for (std::uint32_t l = 0; l < 2; ++l)
        for (std::uint32_t m = 0; m < 4; ++m) {
            auto img = image::decodeLevel(s, l, m);
            REQUIRE(img);
            CHECK(img->width == std::max(1u, 8u >> m));
            CHECK(img->height == std::max(1u, 4u >> m));
            CHECK(img->rgba[0] == 10 * l + m); // A8 is shown as a grey mask
            CHECK(img->rgba[img->rgba.size() - 4] == 10 * l + m);
        }
    CHECK_FALSE(image::decodeLevel(s, 0, 4));
    CHECK_FALSE(image::decodeLevel(s, 2, 0));
}
