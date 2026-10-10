#include "viewer/core/Image.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace viewer::image {

namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a))
           | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8)
           | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16)
           | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

bool isBlockCompressed(PixelFormat f) noexcept
{
    return f == PixelFormat::BC1 || f == PixelFormat::BC2 || f == PixelFormat::BC3 || f == PixelFormat::BC4
           || f == PixelFormat::BC5;
}

std::size_t blockBytes(PixelFormat f) noexcept
{
    return (f == PixelFormat::BC1 || f == PixelFormat::BC4) ? 8 : 16;
}

std::size_t bytesPerPixel(PixelFormat f) noexcept
{
    switch (f) {
    case PixelFormat::A8R8G8B8:
    case PixelFormat::X8R8G8B8:
    case PixelFormat::A8B8G8R8: return 4;
    case PixelFormat::R8G8B8: return 3;
    case PixelFormat::R5G6B5:
    case PixelFormat::A1R5G5B5:
    case PixelFormat::A4R4G4B4:
    case PixelFormat::A8L8: return 2;
    case PixelFormat::L8:
    case PixelFormat::A8: return 1;
    default: return 0;
    }
}

std::uint8_t expand(std::uint32_t value, int bits) noexcept
{
    const std::uint32_t max = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((value * 255u + max / 2u) / max);
}

using Rgba = std::array<std::uint8_t, 4>;

Rgba rgb565(std::uint16_t c) noexcept
{
    return {expand((c >> 11) & 0x1F, 5), expand((c >> 5) & 0x3F, 6), expand(c & 0x1F, 5), 255};
}

/// Decodes a BC1 colour block into 16 RGBA pixels. BC2/BC3 always use the
/// 4-colour mode (`forceFourColor`), BC1 switches on the endpoint order.
void decodeColorBlock(const std::byte* block, bool forceFourColor, std::array<Rgba, 16>& out) noexcept
{
    const auto c0 = loadScalar<std::uint16_t>(ByteSpan(block, 8), 0);
    const auto c1 = loadScalar<std::uint16_t>(ByteSpan(block, 8), 2);
    const auto bits = loadScalar<std::uint32_t>(ByteSpan(block, 8), 4);

    std::array<Rgba, 4> palette{rgb565(c0), rgb565(c1), {}, {}};
    if (forceFourColor || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            palette[2][ch] = static_cast<std::uint8_t>((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
            palette[3][ch] = static_cast<std::uint8_t>((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch)
            palette[2][ch] = static_cast<std::uint8_t>((palette[0][ch] + palette[1][ch]) / 2);
        palette[2][3] = 255;
        palette[3] = {0, 0, 0, 0}; // transparent black
    }
    for (int i = 0; i < 16; ++i)
        out[i] = palette[(bits >> (2 * i)) & 3u];
}

/// Decodes a BC3-style interpolated 8-bit channel block (also BC4/BC5).
void decodeAlphaBlock(const std::byte* block, std::array<std::uint8_t, 16>& out) noexcept
{
    const auto a0 = std::to_integer<std::uint32_t>(block[0]);
    const auto a1 = std::to_integer<std::uint32_t>(block[1]);
    std::array<std::uint32_t, 8> values{a0, a1};
    if (a0 > a1) {
        for (std::uint32_t i = 1; i < 7; ++i)
            values[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (std::uint32_t i = 1; i < 5; ++i)
            values[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        values[6] = 0;
        values[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= std::to_integer<std::uint64_t>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<std::uint8_t>(values[(bits >> (3 * i)) & 7u]);
}

void decodeBlocks(ByteSpan src, PixelFormat format, std::uint32_t width, std::uint32_t height, Image& img)
{
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    const std::size_t stride = blockBytes(format);
    std::array<Rgba, 16> px{};
    std::array<std::uint8_t, 16> a{};
    std::array<std::uint8_t, 16> b{};

    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::byte* block = src.data() + (static_cast<std::size_t>(by) * bw + bx) * stride;
            switch (format) {
            case PixelFormat::BC1: decodeColorBlock(block, false, px); break;
            case PixelFormat::BC2:
                decodeColorBlock(block + 8, true, px);
                for (int i = 0; i < 16; ++i) {
                    const auto byte = std::to_integer<std::uint32_t>(block[i / 2]);
                    px[i][3] = expand((i & 1) ? (byte >> 4) : (byte & 0xF), 4);
                }
                break;
            case PixelFormat::BC3:
                decodeColorBlock(block + 8, true, px);
                decodeAlphaBlock(block, a);
                for (int i = 0; i < 16; ++i)
                    px[i][3] = a[i];
                break;
            case PixelFormat::BC4:
                decodeAlphaBlock(block, a);
                for (int i = 0; i < 16; ++i)
                    px[i] = {a[i], a[i], a[i], 255};
                break;
            case PixelFormat::BC5:
                decodeAlphaBlock(block, a);
                decodeAlphaBlock(block + 8, b);
                for (int i = 0; i < 16; ++i) {
                    // Reconstruct Z so tangent-space normal maps look as expected.
                    const float x = a[i] / 127.5f - 1.0f;
                    const float y = b[i] / 127.5f - 1.0f;
                    const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                    px[i] = {a[i], b[i], static_cast<std::uint8_t>(z * 127.5f + 127.5f), 255};
                }
                break;
            default: return;
            }
            for (std::uint32_t py = 0; py < 4; ++py) {
                const std::uint32_t y = by * 4 + py;
                if (y >= height)
                    break;
                for (std::uint32_t pxi = 0; pxi < 4; ++pxi) {
                    const std::uint32_t x = bx * 4 + pxi;
                    if (x >= width)
                        break;
                    auto* dst = img.rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                    std::copy_n(px[py * 4 + pxi].data(), 4, dst);
                }
            }
        }
    }
}

void decodeLinear(ByteSpan src, PixelFormat format, std::uint32_t width, std::uint32_t height, Image& img)
{
    const std::size_t bpp = bytesPerPixel(format);
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < count; ++i) {
        const std::byte* s = src.data() + i * bpp;
        auto u8 = [&](int k) { return std::to_integer<std::uint8_t>(s[k]); };
        auto u16 = [&] { return static_cast<std::uint16_t>(u8(0) | (u8(1) << 8)); };
        Rgba p{};
        switch (format) {
        case PixelFormat::A8R8G8B8: p = {u8(2), u8(1), u8(0), u8(3)}; break;
        case PixelFormat::X8R8G8B8: p = {u8(2), u8(1), u8(0), 255}; break;
        case PixelFormat::A8B8G8R8: p = {u8(0), u8(1), u8(2), u8(3)}; break;
        case PixelFormat::R8G8B8: p = {u8(2), u8(1), u8(0), 255}; break;
        case PixelFormat::R5G6B5: p = rgb565(u16()); break;
        case PixelFormat::A1R5G5B5: {
            const auto c = u16();
            p = {expand((c >> 10) & 31, 5), expand((c >> 5) & 31, 5), expand(c & 31, 5),
                 static_cast<std::uint8_t>((c & 0x8000) ? 255 : 0)};
            break;
        }
        case PixelFormat::A4R4G4B4: {
            const auto c = u16();
            p = {expand((c >> 8) & 15, 4), expand((c >> 4) & 15, 4), expand(c & 15, 4), expand(c >> 12, 4)};
            break;
        }
        case PixelFormat::L8: p = {u8(0), u8(0), u8(0), 255}; break;
        // Alpha-only data is shown as an opaque greyscale mask so it is visible.
        case PixelFormat::A8: p = {u8(0), u8(0), u8(0), 255}; break;
        case PixelFormat::A8L8: p = {u8(0), u8(0), u8(0), u8(1)}; break;
        default: break;
        }
        std::copy_n(p.data(), 4, img.rgba.data() + i * 4);
    }
}

} // namespace

std::string_view pixelFormatName(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::BC1: return "DXT1 (BC1)";
    case PixelFormat::BC2: return "DXT3 (BC2)";
    case PixelFormat::BC3: return "DXT5 (BC3)";
    case PixelFormat::BC4: return "ATI1 (BC4)";
    case PixelFormat::BC5: return "ATI2 (BC5)";
    case PixelFormat::A8R8G8B8: return "A8R8G8B8";
    case PixelFormat::X8R8G8B8: return "X8R8G8B8";
    case PixelFormat::A8B8G8R8: return "A8B8G8R8";
    case PixelFormat::R8G8B8: return "R8G8B8";
    case PixelFormat::R5G6B5: return "R5G6B5";
    case PixelFormat::A1R5G5B5: return "A1R5G5B5";
    case PixelFormat::A4R4G4B4: return "A4R4G4B4";
    case PixelFormat::L8: return "L8";
    case PixelFormat::A8: return "A8";
    case PixelFormat::A8L8: return "A8L8";
    }
    return "unknown";
}

std::size_t surfaceSize(PixelFormat format, std::uint32_t width, std::uint32_t height) noexcept
{
    if (isBlockCompressed(format))
        return static_cast<std::size_t>(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4)
               * blockBytes(format);
    return static_cast<std::size_t>(width) * height * bytesPerPixel(format);
}

bool isCompressed(PixelFormat format) noexcept
{
    return isBlockCompressed(format);
}

std::size_t chainSize(PixelFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t mipCount) noexcept
{
    std::size_t total = 0;
    for (std::uint32_t m = 0; m < mipCount; ++m) {
        total += surfaceSize(format, width, height);
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return total;
}

std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height) noexcept
{
    std::uint32_t levels = 1;
    for (std::uint32_t size = std::max(width, height); size > 1; size /= 2)
        ++levels;
    return levels;
}

std::uint32_t toD3DFormat(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::BC1: return fourcc('D', 'X', 'T', '1');
    case PixelFormat::BC2: return fourcc('D', 'X', 'T', '3');
    case PixelFormat::BC3: return fourcc('D', 'X', 'T', '5');
    case PixelFormat::BC4: return fourcc('A', 'T', 'I', '1');
    case PixelFormat::BC5: return fourcc('A', 'T', 'I', '2');
    case PixelFormat::R8G8B8: return 20;
    case PixelFormat::A8R8G8B8: return 21;
    case PixelFormat::X8R8G8B8: return 22;
    case PixelFormat::R5G6B5: return 23;
    case PixelFormat::A1R5G5B5: return 25;
    case PixelFormat::A4R4G4B4: return 26;
    case PixelFormat::A8: return 28;
    case PixelFormat::A8B8G8R8: return 32;
    case PixelFormat::L8: return 50;
    case PixelFormat::A8L8: return 51;
    }
    return 0;
}

std::optional<PixelFormat> fromD3DFormat(std::uint32_t v) noexcept
{
    switch (v) {
    case fourcc('D', 'X', 'T', '1'): return PixelFormat::BC1;
    case fourcc('D', 'X', 'T', '2'):
    case fourcc('D', 'X', 'T', '3'): return PixelFormat::BC2;
    case fourcc('D', 'X', 'T', '4'):
    case fourcc('D', 'X', 'T', '5'): return PixelFormat::BC3;
    case fourcc('A', 'T', 'I', '1'):
    case fourcc('B', 'C', '4', 'U'): return PixelFormat::BC4;
    case fourcc('A', 'T', 'I', '2'):
    case fourcc('B', 'C', '5', 'U'): return PixelFormat::BC5;
    // Numeric D3DFORMAT values (d3d9types.h).
    case 20: return PixelFormat::R8G8B8;
    case 21: return PixelFormat::A8R8G8B8;
    case 22: return PixelFormat::X8R8G8B8;
    case 23: return PixelFormat::R5G6B5;
    case 25: return PixelFormat::A1R5G5B5;
    case 26: return PixelFormat::A4R4G4B4;
    case 28: return PixelFormat::A8;
    case 32: return PixelFormat::A8B8G8R8;
    case 50: return PixelFormat::L8;
    case 51: return PixelFormat::A8L8;
    default: return std::nullopt;
    }
}

Result<Image> decodeSurface(ByteSpan pixels, PixelFormat format, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return fail(ErrorCode::InvalidFormat, "invalid image size {}x{}", width, height);
    const std::size_t needed = surfaceSize(format, width, height);
    if (pixels.size() < needed)
        return fail(ErrorCode::OutOfBounds, "{} surface {}x{} needs {} bytes, have {}", pixelFormatName(format),
                    width, height, needed, pixels.size());

    Image img;
    img.width = width;
    img.height = height;
    img.format = std::string(pixelFormatName(format));
    img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    if (isBlockCompressed(format))
        decodeBlocks(pixels, format, width, height, img);
    else
        decodeLinear(pixels, format, width, height, img);
    return img;
}

Result<Surfaces> readDds(ByteSpan file)
{
    constexpr std::size_t kHeader = 128;
    if (file.size() < kHeader || loadScalar<std::uint32_t>(file, 0) != fourcc('D', 'D', 'S', ' '))
        return fail(ErrorCode::InvalidFormat, "not a DDS file");

    auto u32 = [&](std::size_t off) { return loadScalar<std::uint32_t>(file, off); };
    const std::uint32_t height = u32(12);
    const std::uint32_t width = u32(16);
    const std::uint32_t depth = u32(24);
    const std::uint32_t mips = std::max(1u, u32(28));
    const std::uint32_t pfFlags = u32(80);
    const std::uint32_t fcc = u32(84);
    const std::uint32_t bitCount = u32(88);
    const std::uint32_t rMask = u32(92), gMask = u32(96), bMask = u32(100), aMask = u32(104);
    const std::uint32_t caps2 = u32(112);

    std::size_t dataOffset = kHeader;
    std::optional<PixelFormat> format;
    std::uint32_t layers = (caps2 & 0x200) ? 6 : std::max(1u, depth);

    constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40,
                            DDPF_LUMINANCE = 0x20000;

    if (pfFlags & DDPF_FOURCC) {
        if (fcc == fourcc('D', 'X', '1', '0')) {
            if (file.size() < kHeader + 20)
                return fail(ErrorCode::InvalidFormat, "truncated DX10 header");
            dataOffset += 20;
            layers = std::max(1u, u32(kHeader + 12));
            switch (u32(kHeader)) { // DXGI_FORMAT
            case 71: case 72: format = PixelFormat::BC1; break;
            case 74: case 75: format = PixelFormat::BC2; break;
            case 77: case 78: format = PixelFormat::BC3; break;
            case 80: case 81: format = PixelFormat::BC4; break;
            case 83: case 84: format = PixelFormat::BC5; break;
            case 28: case 29: format = PixelFormat::A8B8G8R8; break;
            case 87: case 91: format = PixelFormat::A8R8G8B8; break;
            case 88: case 93: format = PixelFormat::X8R8G8B8; break;
            case 85: format = PixelFormat::R5G6B5; break;
            case 86: format = PixelFormat::A1R5G5B5; break;
            case 61: format = PixelFormat::L8; break;
            case 65: format = PixelFormat::A8; break;
            default: return fail(ErrorCode::Unsupported, "unsupported DXGI format {}", u32(kHeader));
            }
        } else {
            format = fromD3DFormat(fcc);
        }
    } else if (pfFlags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA)) {
        const bool hasAlpha = (pfFlags & DDPF_ALPHAPIXELS) && aMask != 0;
        if (pfFlags & DDPF_LUMINANCE)
            format = (bitCount == 16 && hasAlpha) ? PixelFormat::A8L8 : PixelFormat::L8;
        else if ((pfFlags & DDPF_ALPHA) && bitCount == 8)
            format = PixelFormat::A8;
        else if (bitCount == 32 && rMask == 0x00FF0000 && gMask == 0x0000FF00 && bMask == 0x000000FF)
            format = hasAlpha ? PixelFormat::A8R8G8B8 : PixelFormat::X8R8G8B8;
        else if (bitCount == 32 && rMask == 0x000000FF && bMask == 0x00FF0000)
            format = PixelFormat::A8B8G8R8;
        else if (bitCount == 24)
            format = PixelFormat::R8G8B8;
        else if (bitCount == 16 && rMask == 0xF800)
            format = PixelFormat::R5G6B5;
        else if (bitCount == 16 && rMask == 0x7C00)
            format = PixelFormat::A1R5G5B5;
        else if (bitCount == 16 && rMask == 0x0F00)
            format = PixelFormat::A4R4G4B4;
    }

    if (!format)
        return fail(ErrorCode::Unsupported, "unsupported DDS pixel format (flags {:#x}, fourcc {:#010x}, {} bpp)",
                    pfFlags, fcc, bitCount);

    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return fail(ErrorCode::InvalidFormat, "invalid image size {}x{}", width, height);

    Surfaces s;
    s.format = *format;
    s.width = width;
    s.height = height;
    s.layers = layers;
    const ByteSpan pixels = file.subspan(dataOffset);
    // Keep the mip levels the file really holds (some writers over-report).
    s.mipCount = std::min(mips, fullMipCount(width, height));
    while (s.mipCount > 1 && chainSize(*format, width, height, s.mipCount) * layers > pixels.size())
        --s.mipCount;
    if (s.mipCount == 1 && chainSize(*format, width, height, 1) * layers > pixels.size())
        s.layers = 1; // a short cube map: show its first face
    const std::size_t size = chainSize(*format, width, height, s.mipCount) * s.layers;
    if (pixels.size() < size)
        return fail(ErrorCode::OutOfBounds, "{} surface {}x{} needs {} bytes, have {}", pixelFormatName(*format), width,
                    height, size, pixels.size());
    s.data.assign(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(size));
    return s;
}

Result<Image> decodeDds(ByteSpan file)
{
    auto s = readDds(file);
    if (!s)
        return std::unexpected(s.error());
    auto img = decodeSurface(s->data, s->format, s->width, s->height);
    if (img) {
        // Report what the file says (the header), as before.
        img->mipCount = std::max(1u, loadScalar<std::uint32_t>(file, 28));
        img->layers = s->layers;
    }
    return img;
}


} // namespace viewer::image
