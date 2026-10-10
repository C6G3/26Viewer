#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

namespace viewer::compression {

/// Decompresses a zlib stream (RFC 1950: 2-byte header + deflate + adler32).
/// `expectedSize` is used to size the output exactly; the result is an error
/// if the stream produces fewer bytes.
Result<ByteBuffer> inflateZlib(ByteSpan input, std::size_t expectedSize);

/// Decompresses a headerless deflate stream (RFC 1951), appending to `out`.
Result<void> inflateRawAppend(ByteSpan input, std::size_t expectedSize, ByteBuffer& out);

/// Decompresses a gzip-or-zlib stream when the output size is unknown.
Result<ByteBuffer> inflateZlibUnknownSize(ByteSpan input, std::size_t sizeLimit = std::size_t{1} << 31);

/// True if the first two bytes form a valid zlib header (CMF/FLG check).
[[nodiscard]] bool looksLikeZlib(ByteSpan input) noexcept;

/// Standard CRC-32 (the zlib/PNG polynomial).
[[nodiscard]] std::uint32_t crc32(ByteSpan data) noexcept;

} // namespace viewer::compression
