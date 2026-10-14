// Blur's UI files: fonts (.pf), texture packs (.bpr), font tables (.dat) and vector layouts (.ipf). A small font is built here; with BLUR_DIR set,
// every font in the game is read too.

#include "TestUtil.hpp"

#include "FontFile.hpp"
#include "PakArchive.hpp"
#include "UiFontTable.hpp"
#include "UiLayout.hpp"
#include "UiPack.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <tuple>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <sstream>

using namespace viewer;
using namespace viewer::games::blur;
using Catch::Approx;

namespace {

struct FontWriter : test::Writer {
    void f(float v) { put(std::bit_cast<std::uint32_t>(v)); }
    void point(float x, float y, std::uint8_t kind)
    {
        f(x);
        f(y);
        put(kind);
    }
};

} // namespace

TEST_CASE("Blur fonts: outlines with lines and curves, spacing and kerning")
{
    FontWriter w;
    w.put("Tiny", true);
    while (w.bytes.size() < 0x100)
        w.put<std::uint8_t>(0xCD);
    w.put<std::uint32_t>(2); // glyphs
    // 'H': a square from (0, 90) to (100, 190), lines only.
    w.put<std::uint16_t>('H');
    w.f(10);
    w.f(100);
    w.f(12);
    w.put<std::uint32_t>(1);
    w.put<std::uint32_t>(4);
    w.point(0, 90, 0);
    w.point(100, 90, 0);
    w.point(100, 190, 0);
    w.point(0, 190, 0);
    // 'o': a cubic curve and a quadratic one.
    w.put<std::uint16_t>('o');
    w.f(5);
    w.f(80);
    w.f(5);
    w.put<std::uint32_t>(1);
    w.put<std::uint32_t>(6);
    w.point(0, 130, 0);
    w.point(0, 90, 3);
    w.point(80, 90, 0);
    w.point(80, 130, 0);
    w.point(40, 170, 2);
    w.point(0, 130, 0);
    w.put<std::uint32_t>(1); // kerning pairs
    w.put<std::uint16_t>('H');
    w.put<std::uint16_t>('o');
    w.f(-7.5f);

    const PfFontDecoder decoder;
    REQUIRE(decoder.canDecode("pf", w.bytes));
    auto font = decoder.decode(w.bytes);
    REQUIRE(font);
    CHECK(font->name == "Tiny");
    REQUIRE(font->glyphs.size() == 2);
    const Font::Glyph* h = font->find(U'H');
    REQUIRE(h);
    CHECK(h->advance() == Approx(122));
    REQUIRE(h->contours.size() == 1);
    CHECK(h->contours[0].size() == 4);
    const Font::Glyph* o = font->find(U'o');
    REQUIRE(o);
    CHECK(o->contours[0][1].kind == Font::Point::Kind::Cubic);
    CHECK(o->contours[0][4].kind == Font::Point::Kind::Quadratic);
    CHECK(o->contours[0][2].kind == Font::Point::Kind::On);
    CHECK(font->kern(U'H', U'o') == Approx(-7.5f));
    CHECK(font->kern(U'o', U'H') == 0.0f);
    CHECK(font->baseline == Approx(90)); // the bottom of the 'H'
    CHECK(font->notes.empty());

    // Cut short: an error, not a crash.
    w.bytes.resize(0x130);
    CHECK_FALSE(decoder.decode(w.bytes));
}

TEST_CASE("Real Blur fonts (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "root.pak");
    REQUIRE(pak);
    const auto entries = (*pak)->entries();
    std::size_t fonts = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].path.ends_with(".pf"))
            continue;
        INFO(entries[i].path);
        ++fonts;
        auto font = PfFontDecoder().decode((*pak)->read(i).value());
        REQUIRE(font);
        CHECK(font->notes.empty()); // read to the last byte
        CHECK(font->glyphs.size() >= 75);
        CHECK(font->baseline > 40);
        CHECK(font->lineHeight > 400);
        if (entries[i].path.ends_with("myerssansboldslant.pf")) {
            CHECK(font->name == "MyersSansBoldSlant");
            CHECK(font->glyphs.size() == 291);
            CHECK(font->kerning.size() == 282);
            REQUIRE(font->find(U'g'));
            CHECK(font->find(U'g')->contours.size() == 2);
        }
    }
    CHECK(fonts == 5);
}

TEST_CASE("Blur UI packs: textures listed, read and replaced in place")
{
    // Two textures: an 8x8 DXT1 with two levels (the second 4x4) and a 4x4 greyscale one.
    test::Writer names;
    names.put("first", true);
    names.put("second", true);
    test::Writer records;
    const auto record = [&](bool last, std::uint16_t name, std::uint32_t offset, std::uint32_t size, std::uint16_t format,
                            std::uint16_t mips, std::uint16_t w, std::uint16_t h) {
        records.put<std::uint16_t>(last ? 0 : 28);
        records.put(name);
        records.put(offset);
        records.put<std::uint16_t>(1); // a texture
        records.put<std::uint16_t>(0);
        records.put(size);
        records.put<std::uint32_t>(0);
        records.put(format);
        records.put(mips);
        records.put(w);
        records.put(h);
    };
    record(false, 0, 0, 32 + 8, 1, 4, 8, 8);  // says 4 levels; 2 are stored
    record(true, 6, 40, 16, 7, 1, 4, 4);
    test::Writer w;
    w.put("2RZB");
    w.put<std::uint8_t>(3);
    w.put<std::uint8_t>(2);
    w.put<std::uint16_t>(static_cast<std::uint16_t>(records.bytes.size() + names.bytes.size()));
    w.put<std::uint32_t>(static_cast<std::uint32_t>(records.bytes.size()));
    w.put<std::uint32_t>(56);
    w.put<std::uint32_t>(0);
    w.bytes.insert(w.bytes.end(), records.bytes.begin(), records.bytes.end());
    w.bytes.insert(w.bytes.end(), names.bytes.begin(), names.bytes.end());
    for (int i = 0; i < 56; ++i)
        w.put<std::uint8_t>(static_cast<std::uint8_t>(i));

    const UiPackTextures pack;
    REQUIRE(pack.canRead("bpr", w.bytes));
    auto list = pack.list(w.bytes);
    REQUIRE(list);
    REQUIRE(list->size() == 2);
    CHECK((*list)[0].name == "first");
    CHECK((*list)[0].format == image::PixelFormat::BC1);
    CHECK((*list)[0].mipCount == 2);
    CHECK((*list)[1].name == "second");
    CHECK((*list)[1].format == image::PixelFormat::L8);
    auto grey = pack.surfaces(w.bytes, 1);
    REQUIRE(grey);
    REQUIRE(grey->data.size() == 16);
    CHECK(std::to_integer<int>(grey->data[0]) == 40);

    // Replacing keeps everything else byte for byte.
    image::Surfaces changed = *grey;
    changed.data.assign(16, std::byte{0x7F});
    auto replaced = pack.replace(w.bytes, 1, changed);
    REQUIRE(replaced);
    CHECK(replaced->size() == w.bytes.size());
    CHECK(pack.surfaces(*replaced, 1)->data == changed.data);
    CHECK(pack.surfaces(*replaced, 0)->data == pack.surfaces(w.bytes, 0)->data);
    changed.width = 8; // must stay the same size
    CHECK_FALSE(pack.replace(w.bytes, 1, changed));
}

TEST_CASE("Real Blur UI packs (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "root.pak");
    REQUIRE(pak);
    const auto entries = (*pak)->entries();
    const UiPackTextures format;
    std::size_t packs = 0, textures = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].path.ends_with(".bpr"))
            continue;
        INFO(entries[i].path);
        ++packs;
        const ByteBuffer data = (*pak)->read(i).value();
        auto list = format.list(data);
        REQUIRE(list);
        textures += list->size();
        // Each texture's pixels decode, and putting them back changes nothing.
        for (std::size_t t = 0; t < list->size(); ++t) {
            auto s = format.surfaces(data, t);
            REQUIRE(s);
            CHECK(image::decodeSurface(s->data, s->format, s->width, s->height));
            if (t == 0)
                CHECK(format.replace(data, t, *s).value() == data);
        }
        if (entries[i].path.ends_with("ui_common.bpr")) {
            CHECK(list->size() == 63);
            CHECK((*list)[1].name == "biz_tile");
        }
    }
    CHECK(packs == 4);
    CHECK(textures > 290);

    // The font table for the atlas in ui_font_eur.bpr: three fonts, read to the end.
    const auto table = std::ranges::find_if(entries, [](const auto& e) { return e.path.ends_with("ui_font_eur.dat"); });
    REQUIRE(table != entries.end());
    const ByteBuffer bytes = (*pak)->read(static_cast<std::size_t>(table - entries.begin())).value();
    const UiFontTableDecompiler decompiler;
    REQUIRE(decompiler.canDecompile("dat", bytes));
    auto text = decompiler.decompile(bytes);
    REQUIRE(text);
    CHECK(text->contains("font \"E_Swis712\""));
    CHECK(text->contains("font \"E_myerSansBoldSlant\""));
    CHECK(text->contains("char 'A'"));
    CHECK_FALSE(text->contains("not read"));

    // Edited and written back: the line height, a character's spacing and its atlas place.
    REQUIRE(decompiler.canRecompile());
    auto same = decompiler.recompile(bytes, *text, {});
    REQUIRE(same);
    CHECK(*same == bytes);
    std::string edited = *text;
    const std::size_t a = edited.find("char 'A'");
    const std::size_t lineEnd = edited.find('\n', a);
    std::string line = edited.substr(a, lineEnd - a);
    // "char 'A'     at    x    y    w   h     x_offset  width  advance"
    std::istringstream fields(line.substr(line.find(" at ") + 4));
    long x = 0, y = 0, w = 0, h = 0, offset = 0, width = 0, advance = 0;
    fields >> x >> y >> w >> h >> offset >> width >> advance;
    const std::string changed = std::format("char 'A'     at {} {} {} {}   {}      {}   {}", x + 2, y, w + 1, h, offset,
                                            width, advance + 3);
    edited.replace(a, lineEnd - a, changed);
    auto rebuilt = decompiler.recompile(bytes, edited, {});
    REQUIRE(rebuilt);
    CHECK(rebuilt->size() == bytes.size());
    auto again = decompiler.decompile(*rebuilt);
    REQUIRE(again);
    const std::size_t a2 = again->find("char 'A'");
    std::istringstream after(again->substr(a2 + line.find(" at ") + 4, again->find('\n', a2) - a2));
    long x2 = 0, y2 = 0, w2 = 0, h2 = 0, offset2 = 0, width2 = 0, advance2 = 0;
    after >> x2 >> y2 >> w2 >> h2 >> offset2 >> width2 >> advance2;
    CHECK(x2 == x + 2);
    CHECK(y2 == y);
    CHECK(w2 == w + 1);
    CHECK(h2 == h);
    CHECK(advance2 == advance + 3);
}

TEST_CASE("Blur UI layouts: filled and outlined shapes as triangles, items side by side")
{
    FontWriter w; // little-endian writer with floats
    const auto text = [&](std::string_view s) {
        w.put<std::uint32_t>(static_cast<std::uint32_t>(s.size()));
        w.put(s);
    };
    const auto shape = [&](std::string_view name, bool filled) {
        w.put<std::uint32_t>(1); // a shape
        w.put<std::uint32_t>(7); // id
        text(name);
        w.put<std::uint32_t>(filled ? 1 : 0);
        if (filled) {
            w.f(1);
            w.f(0);
            w.f(0); // red
        }
        w.f(0);
        w.f(0);
        w.f(1); // blue outline
        w.f(filled ? 0 : 4);
        w.put<std::uint8_t>(255);
        w.put<std::uint8_t>(0);
        for (int i = 0; i < 10; ++i)
            w.f(0);
        // A unit square as one fill list (two triangles), or an outline strip of 4 points.
        w.put<std::uint32_t>(filled ? 1 : 0);
        if (filled) {
            w.put<std::uint32_t>(4);
            for (auto [x, y] : {std::pair{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}}) {
                w.f(x);
                w.f(y);
                for (int i = 0; i < 4; ++i)
                    w.f(0);
            }
        }
        w.put<std::uint32_t>(filled ? 1 : 0);
        if (filled) {
            w.put<std::uint32_t>(6);
            for (std::uint16_t i : {0, 1, 2, 0, 2, 3})
                w.put(i);
        }
        w.put<std::uint32_t>(filled ? 0 : 1);
        if (!filled) {
            w.put<std::uint32_t>(4);
            for (auto [x, y, dx] : {std::tuple{0.f, 0.5f, -2.f}, {0.f, 0.5f, 2.f}, {1.f, 0.5f, -2.f}, {1.f, 0.5f, 2.f}}) {
                w.f(x);
                w.f(y);
                w.f(0);
                w.f(dx);
                for (int i = 0; i < 4; ++i)
                    w.f(0);
            }
        }
    };
    w.put<std::uint32_t>(0x00100000);
    w.put<std::uint32_t>(1); // one layer
    w.put<std::uint32_t>(3);
    w.put<std::uint32_t>(0);
    text("layer 1");
    w.put<std::uint32_t>(2); // two items
    shape("box", true);
    shape("line", false);

    const IpfDecoder decoder;
    REQUIRE(decoder.canDecode("ipf", w.bytes));
    auto drawing = decoder.decode(w.bytes);
    REQUIRE(drawing);
    CHECK(drawing->notes.empty());
    REQUIRE(drawing->shapes.size() == 2);
    CHECK(drawing->shapes[0].triangles.size() == 6);
    CHECK(drawing->shapes[0].color[0] == Approx(1));
    CHECK_FALSE(drawing->shapes[0].outline);
    CHECK(drawing->shapes[1].outline);
    CHECK(drawing->shapes[1].triangles.size() == 6); // a strip of 4 points: 2 triangles
    REQUIRE(drawing->labels.size() == 2);
    CHECK(drawing->labels[0].text == "box");
    CHECK(drawing->labels[1].text == "line");
    CHECK(drawing->labels[1].x > drawing->labels[0].x); // side by side
    CHECK(drawing->width > drawing->height);

    w.bytes.resize(w.bytes.size() - 10);
    CHECK_FALSE(decoder.decode(w.bytes));
}

TEST_CASE("Real Blur UI layouts (set BLUR_DIR to enable)")
{
    const char* dir = std::getenv("BLUR_DIR");
    if (!dir) {
        SUCCEED("BLUR_DIR not set; skipping");
        return;
    }
    auto pak = PakArchive::open(std::filesystem::path(dir) / "cache" / "root.pak");
    REQUIRE(pak);
    const auto entries = (*pak)->entries();
    const IpfDecoder decoder;
    std::size_t read = 0, other = 0, drawn = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].path.ends_with(".ipf"))
            continue;
        INFO(entries[i].path);
        auto drawing = decoder.decode((*pak)->read(i).value());
        if (!drawing) {
            CHECK(drawing.error().code == ErrorCode::Unsupported); // console / older versions
            ++other;
            continue;
        }
        ++read;
        CHECK(drawing->notes.empty()); // read to the last byte
        drawn += !drawing->shapes.empty();
    }
    CHECK(read == 71);
    CHECK(drawn == 70); // one minimap holds only its "<bounds>"
    CHECK(other == 4);
}
