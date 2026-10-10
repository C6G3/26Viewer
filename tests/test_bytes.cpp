#include "viewer/core/Archive.hpp"
#include "viewer/core/Bytes.hpp"
#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/FileTypes.hpp"
#include "viewer/core/Vfs.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace viewer;

TEST_CASE("BinaryReader reads little and big endian values with bounds checks")
{
    const std::byte raw[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}};
    BinaryReader r(raw);
    CHECK(r.read<std::uint16_t>().value() == 0x0201);
    r.setEndian(std::endian::big);
    CHECK(r.read<std::uint16_t>().value() == 0x0304);
    CHECK(r.remaining() == 1);

    auto tooFar = r.read<std::uint32_t>();
    REQUIRE_FALSE(tooFar);
    CHECK(tooFar.error().code == ErrorCode::OutOfBounds);
    CHECK_FALSE(r.seek(6));
}

TEST_CASE("cstringAt stops at NUL or end of buffer")
{
    const auto data = asBytes(std::string_view("abc\0def", 7));
    CHECK(cstringAt(data, 0) == "abc");
    CHECK(cstringAt(data, 4) == "def");
    CHECK(cstringAt(data, 99).empty());
}

TEST_CASE("Archive paths are normalised")
{
    CHECK(normalizeArchivePath("\\models\\cars\\a.model") == "models/cars/a.model");
    CHECK(normalizeArchivePath("//x///y/") == "x/y");
    CHECK(normalizeArchivePath("") == "");
}

TEST_CASE("Extraction paths cannot escape the output folder")
{
    CHECK(sanitizeRelativePath("../../windows/system32/evil.dll") == std::filesystem::path("windows/system32/evil.dll"));
    CHECK(sanitizeRelativePath("C:/x.txt") == std::filesystem::path("C_/x.txt"));
    CHECK(sanitizeRelativePath("a/./b//c.txt") == std::filesystem::path("a/b/c.txt"));
}

TEST_CASE("FILETIME conversion")
{
    // 2010-04-29 00:00:00 UTC
    const auto t = fromFileTime(129169728000000000ULL);
    REQUIRE(t);
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(t->time_since_epoch()).count() == 1272499200);
    CHECK_FALSE(fromFileTime(0));
}

TEST_CASE("File types: extension lookup and game overrides")
{
    FileTypeRegistry types;
    types.add({"DDS", "texture", FileCategory::Texture});
    CHECK(extensionOf(std::string_view("dir.v2/file.DDS")) == "dds");
    CHECK(extensionOf(std::string_view("dir.v2/file")).empty());
    CHECK(types.lookup("a/b.dds").category == FileCategory::Texture);
    CHECK(types.lookup("a/b.xyz").category == FileCategory::Unknown);
    types.add({"dds", "override", FileCategory::Data});
    CHECK(types.lookup("b.dds").description == "override");
}

TEST_CASE("Text detection")
{
    CHECK(looksLikeText(asBytes("hello\r\nworld\t!")));
    CHECK_FALSE(looksLikeText(asBytes(std::string_view("bin\0ary", 7))));
    CHECK_FALSE(looksLikeText({}));
}
