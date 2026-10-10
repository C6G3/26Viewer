#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/RandomAccessFile.hpp"

#include <algorithm>
#include <ranges>

namespace viewer {

void ContentRegistry::addArchiveFormat(std::shared_ptr<const ArchiveFormat> format)
{
    m_archiveFormats.push_back(std::move(format));
}

void ContentRegistry::addImageDecoder(std::shared_ptr<const ImageDecoder> decoder)
{
    m_imageDecoders.push_back(std::move(decoder));
}

void ContentRegistry::addStructureParser(std::shared_ptr<const StructureParser> parser)
{
    m_structureParsers.push_back(std::move(parser));
}

void ContentRegistry::addMeshDecoder(std::shared_ptr<const MeshDecoder> decoder)
{
    m_meshDecoders.push_back(std::move(decoder));
}

void ContentRegistry::addSoundBankFormat(std::shared_ptr<const SoundBankFormat> format)
{
    m_soundBankFormats.push_back(std::move(format));
}

const ArchiveFormat* ContentRegistry::findArchiveFormat(const std::filesystem::path& path) const
{
    const auto ext = extensionOf(path);
    ByteBuffer header;
    bool headerLoaded = false;
    for (const auto& format : m_archiveFormats | std::views::reverse) {
        if (!std::ranges::contains(format->extensions(), ext))
            continue;
        if (!headerLoaded) {
            auto bytes = readFile(path, 4096);
            if (!bytes)
                return nullptr;
            header = std::move(*bytes);
            headerLoaded = true;
        }
        if (format->probe(header, path))
            return format.get();
    }
    return nullptr;
}

const ImageDecoder* ContentRegistry::findImageDecoder(std::string_view extension, ByteSpan data) const
{
    for (const auto& d : m_imageDecoders | std::views::reverse)
        if (d->canDecode(extension, data))
            return d.get();
    return nullptr;
}

const StructureParser* ContentRegistry::findStructureParser(std::string_view extension, ByteSpan data) const
{
    for (const auto& p : m_structureParsers | std::views::reverse)
        if (p->canParse(extension, data))
            return p.get();
    return nullptr;
}

const MeshDecoder* ContentRegistry::findMeshDecoder(std::string_view extension, ByteSpan data) const
{
    for (const auto& d : m_meshDecoders | std::views::reverse)
        if (d->canDecode(extension, data))
            return d.get();
    return nullptr;
}

const SoundBankFormat* ContentRegistry::findSoundBankFormat(std::string_view extension, ByteSpan header) const
{
    for (const auto& f : m_soundBankFormats | std::views::reverse)
        if (f->canOpen(extension, header))
            return f.get();
    return nullptr;
}

namespace {

class DdsDecoder final : public ImageDecoder {
public:
    std::string_view name() const noexcept override { return "DirectDraw Surface"; }
    bool canDecode(std::string_view, ByteSpan data) const override
    {
        return data.size() >= 4 && loadScalar<std::uint32_t>(data, 0) == 0x20534444; // "DDS "
    }
    Result<Image> decode(ByteSpan data) const override { return image::decodeDds(data); }
    Result<image::Surfaces> surfaces(ByteSpan data) const override { return image::readDds(data); }
    bool canRebuild() const noexcept override { return true; }
    std::vector<image::PixelFormat> importFormats(ByteSpan data) const override
    {
        using image::PixelFormat;
        std::vector<PixelFormat> formats{PixelFormat::BC1, PixelFormat::BC2, PixelFormat::BC3, PixelFormat::A8R8G8B8};
        if (auto s = image::readDds(data); s && image::canEncode(s->format) && !std::ranges::contains(formats, s->format))
            formats.push_back(s->format);
        return formats;
    }
    Result<ByteBuffer> rebuild(ByteSpan, const image::Surfaces& surfaces) const override
    {
        return image::writeDds(surfaces);
    }
};

} // namespace

void registerBuiltinContent(ContentRegistry& registry)
{
    auto& t = registry.fileTypes();
    t.add({"dds", "DirectDraw Surface texture", FileCategory::Texture});
    t.add({"png", "PNG image", FileCategory::Texture});
    t.add({"jpg", "JPEG image", FileCategory::Texture});
    t.add({"tga", "Targa image", FileCategory::Texture});
    t.add({"bmp", "Bitmap image", FileCategory::Texture});
    for (const char* ext : {"txt", "ini", "cfg", "csv", "tsv", "xml", "json", "lua", "log", "html", "htm", "md",
                            "yaml", "yml", "toml", "bms", "fx", "fxh", "hlsl", "glsl", "bat", "cmd"})
        t.add({ext, "Text document (." + std::string(ext) + ")", FileCategory::Text});
    t.add({"bik", "Bink video", FileCategory::Video});
    t.add({"bk2", "Bink 2 video", FileCategory::Video});
    t.add({"wav", "Wave audio", FileCategory::Audio});
    t.add({"ogg", "Ogg Vorbis audio", FileCategory::Audio});
    t.add({"xwm", "xWMA audio", FileCategory::Audio});
    t.add({"exe", "Application", FileCategory::Executable});
    t.add({"dll", "Dynamic library", FileCategory::Executable});
    t.add({"asi", "ASI plugin", FileCategory::Executable});
    t.add({"zip", "ZIP archive", FileCategory::Archive});

    registry.addImageDecoder(std::make_shared<DdsDecoder>());
    registry.addSoundBankFormat(audio::wavFormat());
}

bool looksLikeText(ByteSpan data) noexcept
{
    if (data.empty())
        return false;
    // UTF-16 / UTF-8 byte order marks.
    if (data.size() >= 2) {
        const auto b0 = std::to_integer<unsigned>(data[0]);
        const auto b1 = std::to_integer<unsigned>(data[1]);
        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF))
            return true;
        if (data.size() >= 3 && b0 == 0xEF && b1 == 0xBB && std::to_integer<unsigned>(data[2]) == 0xBF)
            return true;
    }
    const std::size_t n = std::min<std::size_t>(data.size(), 4096);
    std::size_t suspicious = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = std::to_integer<unsigned>(data[i]);
        if (c == 0)
            return false;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f')
            ++suspicious;
    }
    return suspicious * 100 < n; // allow < 1% stray control bytes
}

} // namespace viewer
