#pragma once

#include "viewer/core/Bytes.hpp"

#include <array>
#include <string>

namespace viewer::hash {

using Sha1Digest = std::array<std::byte, 20>;

/// SHA-1 (FIPS 180-4). Used by formats that store per-file digests; not for security.
[[nodiscard]] Sha1Digest sha1(ByteSpan data) noexcept;

/// Lowercase hex string of a byte range ("a9993e36...").
[[nodiscard]] std::string toHex(ByteSpan data);

} // namespace viewer::hash
