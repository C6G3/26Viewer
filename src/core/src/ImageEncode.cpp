// Writing textures: RGBA → GPU pixel formats (including DXT compression),
// mip-map generation and .dds files. The decoders live in Image.cpp.

#include "viewer/core/Image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <ranges>

namespace viewer::image {

namespace {

using Rgba = std::array<std::uint8_t, 4>;
using Block = std::array<Rgba, 16>;

struct Vec3 {
    float r = 0, g = 0, b = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
Vec3 operator*(Vec3 a, float s) noexcept { return {a.r * s, a.g * s, a.b * s}; }
float dot(Vec3 a, Vec3 b) noexcept { return a.r * b.r + a.g * b.g + a.b * b.b; }

Vec3 toVec(const Rgba& p) noexcept
{
    return {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
}

std::uint8_t expandBits(std::uint32_t value, int bits) noexcept
{
    const std::uint32_t max = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((value * 255u + max / 2u) / max);
}

std::uint32_t quantize(float v, std::uint32_t max) noexcept
{
    return static_cast<std::uint32_t>(std::clamp(std::lround(v * static_cast<float>(max) / 255.0f), 0L,
                                                 static_cast<long>(max)));
}

std::uint16_t to565(Vec3 c) noexcept
{
    return static_cast<std::uint16_t>((quantize(c.r, 31) << 11) | (quantize(c.g, 63) << 5) | quantize(c.b, 31));
}

/// The colour the decoder produces for a 5:6:5 value.
Vec3 from565(std::uint16_t c) noexcept
{
    return {static_cast<float>(expandBits((c >> 11) & 31u, 5)), static_cast<float>(expandBits((c >> 5) & 63u, 6)),
            static_cast<float>(expandBits(c & 31u, 5))};
}

// ---- BC1 colour blocks ----------------------------------------------------------------------

struct ColorFit {
    std::uint16_t c0 = 0, c1 = 0;
    std::uint32_t bits = 0;
    float error = std::numeric_limits<float>::max();
};

/// Encodes the block with endpoints near `a` and `b`. With `threeColor`, see-through
/// pixels (`transparent`) get index 3 (the decoder's transparent black).
ColorFit fitEndpoints(const Block& px, const std::array<bool, 16>& transparent, bool threeColor, Vec3 a, Vec3 b)
{
    ColorFit fit;
    fit.c0 = to565(a);
    fit.c1 = to565(b);
    // The decoder picks the mode from the endpoint order: c0 > c1 is four colours.
    if (threeColor ? fit.c0 > fit.c1 : fit.c0 < fit.c1)
        std::swap(fit.c0, fit.c1);
    const bool four = fit.c0 > fit.c1;

    std::array<Vec3, 4> palette{from565(fit.c0), from565(fit.c1), {}, {}};
    int colours = 3;
    if (four) {
        palette[2] = palette[0] * (2.0f / 3.0f) + palette[1] * (1.0f / 3.0f);
        palette[3] = palette[0] * (1.0f / 3.0f) + palette[1] * (2.0f / 3.0f);
        colours = 4;
    } else {
        palette[2] = (palette[0] + palette[1]) * 0.5f;
    }

    fit.error = 0;
    for (int i = 0; i < 16; ++i) {
        std::uint32_t index = 3;
        if (!(threeColor && transparent[i])) {
            float best = std::numeric_limits<float>::max();
            for (int k = 0; k < colours; ++k) {
                const Vec3 d = toVec(px[i]) - palette[k];
                if (const float e = dot(d, d); e < best) {
                    best = e;
                    index = static_cast<std::uint32_t>(k);
                }
            }
            fit.error += best;
        }
        fit.bits |= index << (2 * i);
    }
    return fit;
}

/// Least-squares endpoints for the indices of `fit` (how far each pixel sits
/// between the two endpoints is fixed; the endpoints move to fit best).
bool refineEndpoints(const Block& px, const std::array<bool, 16>& transparent, bool threeColor, const ColorFit& fit,
                     Vec3& a, Vec3& b)
{
    const bool four = fit.c0 > fit.c1;
    static constexpr std::array<float, 4> kFour{0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f};
    static constexpr std::array<float, 4> kThree{0.0f, 1.0f, 0.5f, 0.0f};
    float aa = 0, ab = 0, bb = 0;
    Vec3 ra, rb;
    for (int i = 0; i < 16; ++i) {
        if (threeColor && transparent[i])
            continue;
        const std::uint32_t index = (fit.bits >> (2 * i)) & 3u;
        const float t = four ? kFour[index] : kThree[index];
        const float s = 1.0f - t;
        aa += s * s;
        ab += s * t;
        bb += t * t;
        ra = ra + toVec(px[i]) * s;
        rb = rb + toVec(px[i]) * t;
    }
    const float det = aa * bb - ab * ab;
    if (std::abs(det) < 1e-6f)
        return false;
    a = (ra * bb - rb * ab) * (1.0f / det);
    b = (rb * aa - ra * ab) * (1.0f / det);
    return true;
}

/// Best-effort BC1 colour block: the colours' main direction (principal axis)
/// gives the first endpoints, then a few least-squares passes improve them.
std::array<std::byte, 8> encodeColorBlock(const Block& px, bool allowTransparency)
{
    std::array<bool, 16> transparent{};
    bool threeColor = false;
    Vec3 mean;
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        transparent[i] = allowTransparency && px[i][3] < 128;
        threeColor |= transparent[i];
        if (!transparent[i]) {
            mean = mean + toVec(px[i]);
            ++count;
        }
    }

    ColorFit best;
    if (count == 0) { // fully see-through: index 3 everywhere
        best.bits = 0xFFFFFFFFu;
    } else {
        mean = mean * (1.0f / static_cast<float>(count));
        // Covariance of the opaque pixels, then its main axis by power iteration.
        float cov[6] = {};
        for (int i = 0; i < 16; ++i) {
            if (transparent[i])
                continue;
            const Vec3 d = toVec(px[i]) - mean;
            cov[0] += d.r * d.r;
            cov[1] += d.r * d.g;
            cov[2] += d.r * d.b;
            cov[3] += d.g * d.g;
            cov[4] += d.g * d.b;
            cov[5] += d.b * d.b;
        }
        Vec3 axis{1.0f, 1.0f, 1.0f};
        for (int it = 0; it < 8; ++it) {
            const Vec3 next{cov[0] * axis.r + cov[1] * axis.g + cov[2] * axis.b,
                            cov[1] * axis.r + cov[3] * axis.g + cov[4] * axis.b,
                            cov[2] * axis.r + cov[4] * axis.g + cov[5] * axis.b};
            const float len = std::sqrt(dot(next, next));
            if (len < 1e-6f)
                break;
            axis = next * (1.0f / len);
        }
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        for (int i = 0; i < 16; ++i) {
            if (transparent[i])
                continue;
            const float t = dot(toVec(px[i]) - mean, axis);
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        Vec3 a = mean + axis * hi, b = mean + axis * lo;
        best = fitEndpoints(px, transparent, threeColor, a, b);
        for (int pass = 0; pass < 3 && best.error > 0; ++pass) {
            if (!refineEndpoints(px, transparent, threeColor, best, a, b))
                break;
            const ColorFit next = fitEndpoints(px, transparent, threeColor, a, b);
            if (next.error >= best.error)
                break;
            best = next;
        }
    }

    std::array<std::byte, 8> out{};
    std::memcpy(out.data(), &best.c0, 2);
    std::memcpy(out.data() + 2, &best.c1, 2);
    std::memcpy(out.data() + 4, &best.bits, 4);
    return out;
}

// ---- BC3 alpha blocks -----------------------------------------------------------------------

struct AlphaFit {
    std::uint8_t a0 = 0, a1 = 0;
    std::uint64_t bits = 0;
    int error = std::numeric_limits<int>::max();
};

AlphaFit fitAlpha(const std::array<std::uint8_t, 16>& alpha, std::uint8_t a0, std::uint8_t a1)
{
    AlphaFit fit{a0, a1, 0, 0};
    std::array<int, 8> values{a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            values[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i < 5; ++i)
            values[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        values[6] = 0;
        values[7] = 255;
    }
    for (int i = 0; i < 16; ++i) {
        int best = std::numeric_limits<int>::max();
        std::uint64_t index = 0;
        for (int k = 0; k < 8; ++k) {
            const int d = alpha[i] - values[k];
            if (d * d < best) {
                best = d * d;
                index = static_cast<std::uint64_t>(k);
            }
        }
        fit.error += best;
        fit.bits |= index << (3 * i);
    }
    return fit;
}

/// BC3 alpha: tries the 8-step ramp between the extremes and the 6-step ramp
/// (which also has exact 0 and 255) and keeps the closer one.
std::array<std::byte, 8> encodeAlphaBlock(const std::array<std::uint8_t, 16>& alpha)
{
    const auto [lo, hi] = std::minmax_element(alpha.begin(), alpha.end());
    AlphaFit best = fitAlpha(alpha, *hi, *lo);
    int inLo = 255, inHi = 0;
    for (const std::uint8_t a : alpha) {
        if (a != 0 && a != 255) {
            inLo = std::min<int>(inLo, a);
            inHi = std::max<int>(inHi, a);
        }
    }
    if (inLo > inHi)
        inLo = inHi = 0;
    if (const AlphaFit six = fitAlpha(alpha, static_cast<std::uint8_t>(inLo), static_cast<std::uint8_t>(inHi));
        six.error < best.error)
        best = six;

    std::array<std::byte, 8> out{std::byte{best.a0}, std::byte{best.a1}};
    for (int i = 0; i < 6; ++i)
        out[2 + i] = static_cast<std::byte>((best.bits >> (8 * i)) & 0xFFu);
    return out;
}

// ---- Surfaces -------------------------------------------------------------------------------

Block readBlock(const Image& image, std::uint32_t bx, std::uint32_t by)
{
    Block block{};
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            // Edge blocks of odd-sized images repeat the last row/column.
            const std::uint32_t sx = std::min(bx * 4 + x, image.width - 1);
            const std::uint32_t sy = std::min(by * 4 + y, image.height - 1);
            const auto* p = image.rgba.data() + (static_cast<std::size_t>(sy) * image.width + sx) * 4;
            std::copy_n(p, 4, block[y * 4 + x].data());
        }
    }
    return block;
}

void encodeBlocks(const Image& image, PixelFormat format, ByteBuffer& out)
{
    const std::uint32_t bw = std::max(1u, (image.width + 3) / 4);
    const std::uint32_t bh = std::max(1u, (image.height + 3) / 4);
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const Block px = readBlock(image, bx, by);
            std::array<std::uint8_t, 16> alpha{};
            for (int i = 0; i < 16; ++i)
                alpha[i] = px[i][3];
            if (format == PixelFormat::BC2) {
                for (int i = 0; i < 16; i += 2) {
                    const std::uint32_t lo = quantize(alpha[i], 15), hi = quantize(alpha[i + 1], 15);
                    out.push_back(static_cast<std::byte>(lo | (hi << 4)));
                }
            } else if (format == PixelFormat::BC3) {
                const auto a = encodeAlphaBlock(alpha);
                out.insert(out.end(), a.begin(), a.end());
            }
            const auto c = encodeColorBlock(px, format == PixelFormat::BC1);
            out.insert(out.end(), c.begin(), c.end());
        }
    }
}

std::uint8_t luminance(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint8_t>((299 * p[0] + 587 * p[1] + 114 * p[2] + 500) / 1000);
}

} // namespace

bool canEncode(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::BC1:
    case PixelFormat::BC2:
    case PixelFormat::BC3:
    case PixelFormat::A8R8G8B8:
    case PixelFormat::X8R8G8B8:
    case PixelFormat::A8B8G8R8:
    case PixelFormat::R8G8B8:
    case PixelFormat::R5G6B5:
    case PixelFormat::L8:
    case PixelFormat::A8: return true;
    default: return false;
    }
}

Result<ByteBuffer> encodeSurface(const Image& image, PixelFormat format)
{
    if (image.width == 0 || image.height == 0
        || image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4)
        return fail(ErrorCode::InvalidFormat, "invalid image {}x{}", image.width, image.height);
    if (!canEncode(format))
        return fail(ErrorCode::Unsupported, "can't write {} pixels", pixelFormatName(format));

    ByteBuffer out;
    out.reserve(surfaceSize(format, image.width, image.height));
    if (isCompressed(format)) {
        encodeBlocks(image, format, out);
        return out;
    }
    // A8 keeps the alpha channel; an opaque picture (e.g. an exported A8 mask,
    // shown as grey) gives its brightness instead.
    const bool hasAlpha = std::ranges::any_of(image.rgba | std::views::drop(3) | std::views::stride(4),
                                              [](std::uint8_t a) { return a != 255; });
    auto put = [&](std::uint32_t v) { out.push_back(static_cast<std::byte>(v & 0xFFu)); };
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        const std::uint8_t* p = image.rgba.data() + i;
        switch (format) {
        case PixelFormat::A8R8G8B8: put(p[2]), put(p[1]), put(p[0]), put(p[3]); break;
        case PixelFormat::X8R8G8B8: put(p[2]), put(p[1]), put(p[0]), put(255); break;
        case PixelFormat::A8B8G8R8: put(p[0]), put(p[1]), put(p[2]), put(p[3]); break;
        case PixelFormat::R8G8B8: put(p[2]), put(p[1]), put(p[0]); break;
        case PixelFormat::R5G6B5: {
            const std::uint32_t c = (quantize(p[0], 31) << 11) | (quantize(p[1], 63) << 5) | quantize(p[2], 31);
            put(c), put(c >> 8);
            break;
        }
        case PixelFormat::L8: put(luminance(p)); break;
        case PixelFormat::A8: put(hasAlpha ? p[3] : luminance(p)); break;
        default: break;
        }
    }
    return out;
}

Image halfSize(const Image& image)
{
    Image half;
    half.width = std::max(1u, image.width / 2);
    half.height = std::max(1u, image.height / 2);
    half.format = image.format;
    half.rgba.resize(static_cast<std::size_t>(half.width) * half.height * 4);
    for (std::uint32_t y = 0; y < half.height; ++y) {
        for (std::uint32_t x = 0; x < half.width; ++x) {
            std::array<std::uint32_t, 4> sum{};
            std::uint32_t n = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = x * 2 + dx, sy = y * 2 + dy;
                    if (sx >= image.width || sy >= image.height)
                        continue;
                    const auto* p = image.rgba.data() + (static_cast<std::size_t>(sy) * image.width + sx) * 4;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += p[c];
                    ++n;
                }
            }
            auto* d = half.rgba.data() + (static_cast<std::size_t>(y) * half.width + x) * 4;
            for (int c = 0; c < 4; ++c)
                d[c] = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
        }
    }
    return half;
}

Result<Surfaces> encodeSurfaces(const Image& image, PixelFormat format, std::uint32_t mipCount)
{
    Surfaces s;
    s.format = format;
    s.width = image.width;
    s.height = image.height;
    s.mipCount = std::clamp(mipCount, 1u, fullMipCount(image.width, image.height));
    s.data.reserve(chainSize(format, s.width, s.height, s.mipCount));
    Image level = image;
    for (std::uint32_t m = 0; m < s.mipCount; ++m) {
        if (m > 0)
            level = halfSize(level);
        auto bytes = encodeSurface(level, format);
        if (!bytes)
            return std::unexpected(bytes.error());
        s.data.insert(s.data.end(), bytes->begin(), bytes->end());
    }
    return s;
}

Result<ByteBuffer> writeDds(const Surfaces& s)
{
    if (s.layers != 1 && s.layers != 6)
        return fail(ErrorCode::Unsupported, "can't write a .dds with {} layers", s.layers);
    const std::size_t expected = chainSize(s.format, s.width, s.height, s.mipCount) * s.layers;
    if (s.data.size() < expected)
        return fail(ErrorCode::OutOfBounds, "texture data is {} bytes, {} needed", s.data.size(), expected);

    // DDS_PIXELFORMAT: flags, FourCC, bits, R/G/B/A masks
    std::uint32_t pf[6] = {};
    constexpr std::uint32_t ALPHAPIXELS = 0x1, ALPHA = 0x2, FOURCC = 0x4, RGB = 0x40, LUMINANCE = 0x20000;
    switch (s.format) {
    case PixelFormat::BC1:
    case PixelFormat::BC2:
    case PixelFormat::BC3:
    case PixelFormat::BC4:
    case PixelFormat::BC5: pf[0] = FOURCC, pf[1] = toD3DFormat(s.format); break;
    case PixelFormat::A8R8G8B8: pf[0] = RGB | ALPHAPIXELS, pf[2] = 32, pf[3] = 0xFF0000, pf[4] = 0xFF00, pf[5] = 0xFF; break;
    case PixelFormat::X8R8G8B8: pf[0] = RGB, pf[2] = 32, pf[3] = 0xFF0000, pf[4] = 0xFF00, pf[5] = 0xFF; break;
    case PixelFormat::A8B8G8R8: pf[0] = RGB | ALPHAPIXELS, pf[2] = 32, pf[3] = 0xFF, pf[4] = 0xFF00, pf[5] = 0xFF0000; break;
    case PixelFormat::R8G8B8: pf[0] = RGB, pf[2] = 24, pf[3] = 0xFF0000, pf[4] = 0xFF00, pf[5] = 0xFF; break;
    case PixelFormat::R5G6B5: pf[0] = RGB, pf[2] = 16, pf[3] = 0xF800, pf[4] = 0x07E0, pf[5] = 0x001F; break;
    case PixelFormat::A1R5G5B5: pf[0] = RGB | ALPHAPIXELS, pf[2] = 16, pf[3] = 0x7C00, pf[4] = 0x03E0, pf[5] = 0x001F; break;
    case PixelFormat::A4R4G4B4: pf[0] = RGB | ALPHAPIXELS, pf[2] = 16, pf[3] = 0x0F00, pf[4] = 0x00F0, pf[5] = 0x000F; break;
    case PixelFormat::L8: pf[0] = LUMINANCE, pf[2] = 8, pf[3] = 0xFF; break;
    case PixelFormat::A8: pf[0] = ALPHA, pf[2] = 8; break;
    case PixelFormat::A8L8: pf[0] = LUMINANCE | ALPHAPIXELS, pf[2] = 16, pf[3] = 0xFF; break;
    }
    std::uint32_t alphaMask = 0;
    switch (s.format) {
    case PixelFormat::A8R8G8B8:
    case PixelFormat::A8B8G8R8: alphaMask = 0xFF000000u; break;
    case PixelFormat::A1R5G5B5: alphaMask = 0x8000; break;
    case PixelFormat::A4R4G4B4: alphaMask = 0xF000; break;
    case PixelFormat::A8: alphaMask = 0xFF; break;
    case PixelFormat::A8L8: alphaMask = 0xFF00; break;
    default: break;
    }

    constexpr std::uint32_t CAPS = 0x1, HEIGHT = 0x2, WIDTH = 0x4, PITCH = 0x8, PIXELFORMAT = 0x1000,
                            MIPMAPCOUNT = 0x20000, LINEARSIZE = 0x80000;
    const bool compressed = isCompressed(s.format);
    std::uint32_t flags = CAPS | HEIGHT | WIDTH | PIXELFORMAT | (compressed ? LINEARSIZE : PITCH);
    if (s.mipCount > 1)
        flags |= MIPMAPCOUNT;
    const auto pitch = static_cast<std::uint32_t>(compressed ? surfaceSize(s.format, s.width, s.height)
                                                             : surfaceSize(s.format, s.width, 1));
    std::uint32_t caps = 0x1000; // TEXTURE
    if (s.mipCount > 1)
        caps |= 0x8 | 0x400000; // COMPLEX | MIPMAP
    std::uint32_t caps2 = 0;
    if (s.layers == 6) {
        caps |= 0x8;
        caps2 = 0x200 | 0xFC00; // CUBEMAP with all six faces
    }

    std::array<std::uint32_t, 32> header{};
    header[0] = 0x20534444; // "DDS "
    header[1] = 124;
    header[2] = flags;
    header[3] = s.height;
    header[4] = s.width;
    header[5] = pitch;
    header[7] = s.mipCount;
    header[19] = 32; // DDS_PIXELFORMAT size
    header[20] = pf[0];
    header[21] = pf[1];
    header[22] = pf[2];
    header[23] = pf[3];
    header[24] = pf[4];
    header[25] = pf[5];
    header[26] = alphaMask;
    header[27] = caps;
    header[28] = caps2;

    ByteBuffer out(sizeof(header) + expected);
    std::memcpy(out.data(), header.data(), sizeof(header));
    std::memcpy(out.data() + sizeof(header), s.data.data(), expected);
    return out;
}

} // namespace viewer::image
