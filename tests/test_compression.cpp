#include "TestUtil.hpp"

#include "viewer/core/Compression.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace viewer;

TEST_CASE("zlib round trip")
{
    const auto data = test::sampleData(100'000);
    const auto packed = test::deflate(data, true);
    CHECK(compression::looksLikeZlib(packed));
    auto out = compression::inflateZlib(packed, data.size());
    REQUIRE(out);
    CHECK(*out == data);
}

TEST_CASE("raw deflate appends to an existing buffer")
{
    const auto a = test::sampleData(3000, 1);
    const auto b = test::sampleData(5000, 2);
    ByteBuffer out = a;
    REQUIRE(compression::inflateRawAppend(test::deflate(b, false), b.size(), out));
    REQUIRE(out.size() == a.size() + b.size());
    CHECK(std::equal(b.begin(), b.end(), out.begin() + static_cast<std::ptrdiff_t>(a.size())));
}

TEST_CASE("corrupt or short streams are reported, not crashed on")
{
    const auto data = test::sampleData(1000);
    auto packed = test::deflate(data, true);
    CHECK_FALSE(compression::inflateZlib(packed, data.size() + 10)); // too short
    packed.resize(packed.size() / 2);
    CHECK_FALSE(compression::inflateZlib(packed, data.size())); // truncated
}

TEST_CASE("Our compressors round-trip through our decompressors")
{
    const auto data = test::sampleData(70'000, 7);
    auto z = compression::deflateZlib(data);
    REQUIRE(z);
    CHECK(compression::looksLikeZlib(*z));
    CHECK(compression::inflateZlib(*z, data.size()).value() == data);

    ByteBuffer raw;
    REQUIRE(compression::deflateRawAppend(data, raw));
    ByteBuffer out;
    REQUIRE(compression::inflateRawAppend(raw, data.size(), out));
    CHECK(out == data);
}

TEST_CASE("crc32 matches the standard check value")
{
    CHECK(compression::crc32(asBytes("123456789")) == 0xCBF43926u);
}
