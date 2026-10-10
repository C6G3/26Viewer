#include "viewer/core/Hash.hpp"

#include <bit>
#include <cstdint>
#include <format>

namespace viewer::hash {

Sha1Digest sha1(ByteSpan data) noexcept
{
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    auto processBlock = [&h](const std::uint8_t* block) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16)
                   | (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
        for (int i = 16; i < 80; ++i)
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t full = data.size() / 64 * 64;
    for (std::size_t i = 0; i < full; i += 64)
        processBlock(bytes + i);

    // Final block(s): remaining bytes, 0x80, zero padding, 64-bit big-endian bit length.
    std::uint8_t tail[128] = {};
    const std::size_t rest = data.size() - full;
    for (std::size_t i = 0; i < rest; ++i)
        tail[i] = bytes[full + i];
    tail[rest] = 0x80;
    const std::size_t tailLen = rest + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i)
        tail[tailLen - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    processBlock(tail);
    if (tailLen == 128)
        processBlock(tail + 64);

    Sha1Digest out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::byte>(h[i] >> (24 - 8 * j));
    return out;
}

std::string toHex(ByteSpan data)
{
    std::string out;
    out.reserve(data.size() * 2);
    for (std::byte b : data)
        out += std::format("{:02x}", std::to_integer<unsigned>(b));
    return out;
}

} // namespace viewer::hash
