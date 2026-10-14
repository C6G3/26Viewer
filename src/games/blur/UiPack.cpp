// Blur's UI resource packs (.bpr; little-endian). From Blur.exe's loader
// (0xD531E0: "2RZB" then a version byte; version 3 at 0xD53660):
//
//   "2RZB", u8 version (3), u8 flags (low 4 bits 1: the data is compressed —
//   no pack in the game is), u16 table size, u32 records size, u32 data size,
//   u32 0
//   20     records, chained: each starts with u16 size (to the next; 0 on the
//          last), u16 name (offset into the names), and has its type at +8.
//          Type 1, a texture (28 bytes): u32 data offset, u16 1, u16 ?,
//          u32 data size, u32 0, u16 format, u16 mip levels, u16 width, u16 height
//   20 + records size   names (NUL-terminated)
//   20 + table size     the textures' pixels, at their offsets
//
// Formats: 1 DXT1, 5 DXT5, 6 A8R8G8B8, 7 L8. Compressed (DXT) mip chains stop
// at the last level 4 pixels across, so a texture may hold fewer levels than
// it says. Every pack in the game reads (3 menus/HUD packs and the font atlas).

#include "UiPack.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>

namespace viewer::games::blur {

namespace {

constexpr std::size_t kHeader = 20;

struct Texture {
    std::string name;
    std::size_t offset = 0; ///< In the file.
    std::size_t size = 0;
    std::size_t record = 0; ///< Where its record is in the file.
    image::PixelFormat format = image::PixelFormat::BC1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mips = 1; ///< Levels actually stored.
};

std::optional<image::PixelFormat> pixelFormat(std::uint16_t code)
{
    switch (code) {
    case 1: return image::PixelFormat::BC1;
    case 5: return image::PixelFormat::BC3;
    case 6: return image::PixelFormat::A8R8G8B8;
    case 7: return image::PixelFormat::L8;
    default: return std::nullopt;
    }
}

Result<std::vector<Texture>> readPack(ByteSpan d)
{
    if (d.size() < kHeader || std::memcmp(d.data(), "2RZB", 4) != 0)
        return fail(ErrorCode::InvalidFormat, "not a Blur UI pack (no \"2RZB\")");
    const auto version = std::to_integer<std::uint8_t>(d[4]);
    const auto flags = std::to_integer<std::uint8_t>(d[5]);
    if (version != 3)
        return fail(ErrorCode::Unsupported, "UI pack version {} (only 3 is known)", version);
    if ((flags & 0xF) == 1)
        return fail(ErrorCode::Unsupported, "compressed UI packs aren't supported yet");
    const std::size_t table = loadScalar<std::uint16_t>(d, 6);
    const std::size_t records = loadScalar<std::uint32_t>(d, 8);
    const std::size_t names = kHeader + records, data = kHeader + table;
    if (records > table || data > d.size())
        return fail(ErrorCode::InvalidFormat, "UI pack table runs past the end of the file");

    std::vector<Texture> out;
    for (std::size_t p = kHeader;;) {
        if (p + 28 > names)
            return fail(ErrorCode::InvalidFormat, "UI pack record at byte {} runs past the table", p);
        const std::size_t size = loadScalar<std::uint16_t>(d, p);
        if (loadScalar<std::uint16_t>(d, p + 8) == 1) { // a texture
            Texture t;
            t.record = p;
            const std::size_t nameAt = names + loadScalar<std::uint16_t>(d, p + 2);
            for (std::size_t i = nameAt; i < data && d[i] != std::byte{0}; ++i)
                t.name.push_back(static_cast<char>(d[i]));
            t.offset = data + loadScalar<std::uint32_t>(d, p + 4);
            t.size = loadScalar<std::uint32_t>(d, p + 12);
            const auto format = pixelFormat(loadScalar<std::uint16_t>(d, p + 20));
            if (!format)
                return fail(ErrorCode::Unsupported, "texture '{}' has an unknown pixel format {}", t.name,
                            loadScalar<std::uint16_t>(d, p + 20));
            t.format = *format;
            const std::uint32_t mips = std::max<std::uint16_t>(1, loadScalar<std::uint16_t>(d, p + 22));
            t.width = loadScalar<std::uint16_t>(d, p + 24);
            t.height = loadScalar<std::uint16_t>(d, p + 26);
            if (t.offset > d.size() || t.size > d.size() - t.offset)
                return fail(ErrorCode::InvalidFormat, "texture '{}' runs past the end of the file", t.name);
            // As many levels as the stored bytes hold (compressed chains end at 4 pixels).
            t.mips = 0;
            for (std::uint32_t n = mips; n >= 1 && !t.mips; --n)
                if (image::chainSize(t.format, t.width, t.height, n) == t.size)
                    t.mips = n;
            if (!t.mips)
                return fail(ErrorCode::InvalidFormat, "texture '{}' ({}x{}, {} levels) doesn't fill its {} bytes",
                            t.name, t.width, t.height, mips, t.size);
            out.push_back(std::move(t));
        }
        if (size == 0)
            break;
        p += size;
    }
    return out;
}

} // namespace

bool UiPackTextures::canRead(std::string_view extension, ByteSpan data) const
{
    return extension == "bpr" && data.size() >= 4 && std::memcmp(data.data(), "2RZB", 4) == 0;
}

Result<std::vector<DictionaryTexture>> UiPackTextures::list(ByteSpan data) const
{
    auto pack = readPack(data);
    if (!pack)
        return std::unexpected(pack.error());
    std::vector<DictionaryTexture> out;
    for (const Texture& t : *pack)
        out.push_back({t.name, t.format, t.width, t.height, t.mips});
    return out;
}

Result<image::Surfaces> UiPackTextures::surfaces(ByteSpan data, std::size_t index) const
{
    auto pack = readPack(data);
    if (!pack)
        return std::unexpected(pack.error());
    if (index >= pack->size())
        return fail(ErrorCode::NotFound, "no texture #{} in this pack ({} textures)", index + 1, pack->size());
    const Texture& t = (*pack)[index];
    const auto bytes = data.subspan(t.offset, t.size);
    return image::Surfaces{t.format, t.width, t.height, t.mips, 1, ByteBuffer(bytes.begin(), bytes.end())};
}

Result<ByteBuffer> UiPackTextures::replace(ByteSpan data, std::size_t index, const image::Surfaces& s) const
{
    auto pack = readPack(data);
    if (!pack)
        return std::unexpected(pack.error());
    if (index >= pack->size())
        return fail(ErrorCode::NotFound, "no texture #{} in this pack ({} textures)", index + 1, pack->size());
    const Texture& t = (*pack)[index];
    if (s.format != t.format || s.width != t.width || s.height != t.height || s.mipCount != t.mips
        || s.data.size() != t.size)
        return fail(ErrorCode::InvalidFormat, "'{}' must stay {}x{} {} with {} level(s)", t.name, t.width, t.height,
                    image::pixelFormatName(t.format), t.mips);
    ByteBuffer out(data.begin(), data.end());
    std::copy(s.data.begin(), s.data.end(), out.begin() + static_cast<std::ptrdiff_t>(t.offset));
    return out;
}

} // namespace viewer::games::blur
