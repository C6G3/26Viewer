#include "viewer/core/Compression.hpp"

#include <zlib.h>

#include <algorithm>
#include <limits>

namespace viewer::compression {

namespace {

/// Shared driver for zlib (windowBits 15), raw deflate (-15) and auto (15+32).
Result<void> runInflate(ByteSpan input, int windowBits, std::size_t expectedSize, ByteBuffer& out)
{
    if (input.size() > std::numeric_limits<uInt>::max())
        return fail(ErrorCode::Unsupported, "compressed block too large ({} bytes)", input.size());

    z_stream zs{};
    if (inflateInit2(&zs, windowBits) != Z_OK)
        return fail(ErrorCode::Decompression, "inflateInit2 failed");

    const std::size_t base = out.size();
    out.resize(base + expectedSize);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data() + base);
    zs.avail_out = static_cast<uInt>(expectedSize);

    const int rc = inflate(&zs, Z_FINISH);
    const std::size_t produced = expectedSize - zs.avail_out;
    const char* message = zs.msg;
    inflateEnd(&zs);

    // Z_BUF_ERROR with a full output buffer just means trailing data remains
    // (common when stored sizes are padded); the payload itself is complete.
    const bool complete = rc == Z_STREAM_END || (rc == Z_BUF_ERROR && produced == expectedSize)
                          || (rc == Z_OK && produced == expectedSize);
    if (!complete) {
        out.resize(base);
        return fail(ErrorCode::Decompression, "inflate failed ({}): {}", rc, message ? message : "corrupt stream");
    }
    if (produced != expectedSize) {
        out.resize(base);
        return fail(ErrorCode::Decompression, "stream ended after {} of {} bytes", produced, expectedSize);
    }
    return {};
}

} // namespace

Result<ByteBuffer> inflateZlib(ByteSpan input, std::size_t expectedSize)
{
    ByteBuffer out;
    if (auto r = runInflate(input, 15, expectedSize, out); !r)
        return std::unexpected(r.error());
    return out;
}

Result<void> inflateRawAppend(ByteSpan input, std::size_t expectedSize, ByteBuffer& out)
{
    return runInflate(input, -15, expectedSize, out);
}

Result<ByteBuffer> inflateZlibUnknownSize(ByteSpan input, std::size_t sizeLimit)
{
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) // +32: auto-detect zlib or gzip header
        return fail(ErrorCode::Decompression, "inflateInit2 failed");

    ByteBuffer out;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (out.size() >= sizeLimit) {
            inflateEnd(&zs);
            return fail(ErrorCode::Decompression, "output exceeds limit of {} bytes", sizeLimit);
        }
        const std::size_t grow = std::max<std::size_t>(input.size() * 2, 64 * 1024);
        const std::size_t base = out.size();
        out.resize(base + grow);
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + base);
        zs.avail_out = static_cast<uInt>(grow);
        rc = inflate(&zs, Z_NO_FLUSH);
        out.resize(base + (grow - zs.avail_out));
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END)
        return fail(ErrorCode::Decompression, "inflate failed ({})", rc);
    return out;
}

namespace {

Result<void> runDeflate(ByteSpan input, int windowBits, int level, ByteBuffer& out)
{
    if (input.size() > std::numeric_limits<uInt>::max())
        return fail(ErrorCode::Unsupported, "block too large to compress ({} bytes)", input.size());
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, windowBits, 9, Z_DEFAULT_STRATEGY) != Z_OK)
        return fail(ErrorCode::Decompression, "deflateInit2 failed");
    const std::size_t base = out.size();
    const std::size_t bound = deflateBound(&zs, static_cast<uLong>(input.size()));
    out.resize(base + bound);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data() + base);
    zs.avail_out = static_cast<uInt>(bound);
    const int rc = deflate(&zs, Z_FINISH);
    const std::size_t produced = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        out.resize(base);
        return fail(ErrorCode::Decompression, "deflate failed ({})", rc);
    }
    out.resize(base + produced);
    return {};
}

} // namespace

Result<ByteBuffer> deflateZlib(ByteSpan input, int level)
{
    ByteBuffer out;
    if (auto r = runDeflate(input, 15, level, out); !r)
        return std::unexpected(r.error());
    return out;
}

Result<void> deflateRawAppend(ByteSpan input, ByteBuffer& out, int level)
{
    return runDeflate(input, -15, level, out);
}

bool looksLikeZlib(ByteSpan input) noexcept
{
    if (input.size() < 2)
        return false;
    const auto cmf = std::to_integer<unsigned>(input[0]);
    const auto flg = std::to_integer<unsigned>(input[1]);
    return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0;
}

std::uint32_t crc32(ByteSpan data) noexcept
{
    uLong crc = ::crc32(0L, Z_NULL, 0);
    std::size_t done = 0;
    while (done < data.size()) {
        const auto chunk = static_cast<uInt>(std::min<std::size_t>(data.size() - done, 1u << 30));
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(data.data() + done), chunk);
        done += chunk;
    }
    return static_cast<std::uint32_t>(crc);
}

} // namespace viewer::compression
