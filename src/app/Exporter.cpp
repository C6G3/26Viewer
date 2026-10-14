#include "Exporter.hpp"

#include "QtConvert.hpp"

#include "viewer/core/Audio.hpp"
#include "viewer/core/Mesh.hpp"
#include "viewer/core/RandomAccessFile.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QPromise>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <map>

namespace viewer::app {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Exporter", text);
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The bank format for `node`, if it is a real multi-sound bank (a plain .wav isn't).
const SoundBankFormat* bankFormat(const Session& session, const VfsNode& node, ByteSpan header)
{
    const std::string ext = extensionOf(node.name());
    if (ext == "wav")
        return nullptr;
    return session.content.findSoundBankFormat(ext, header);
}

Result<ByteBuffer> head(const Session& session, const VfsNode& node)
{
    return session.vfs.read(node, 4096);
}

/// `relative` with its extension replaced (or removed when `extension` is empty).
std::filesystem::path withExtension(const std::filesystem::path& relative, const char* extension)
{
    std::filesystem::path p = relative;
    p.replace_extension(extension);
    return p;
}

QImage toQImage(const Image& img)
{
    return QImage(img.rgba.data(), static_cast<int>(img.width), static_cast<int>(img.height),
                  static_cast<qsizetype>(img.width) * 4, QImage::Format_RGBA8888);
}

/// Writes a texture in the picture format the user chose.
Result<int> exportTexture(const ImageDecoder& decoder, ByteSpan data, std::string_view ext,
                          const std::filesystem::path& base, ImageExport format, bool& converted)
{
    if (format == ImageExport::Dds) {
        if (ext == "dds") { // already one: copy it as is
            converted = false;
            if (auto ok = writeFile(withExtension(base, ".dds"), data); !ok)
                return std::unexpected(ok.error());
            return 1;
        }
        // The game's own pixels and mip-maps, unchanged — the best source for
        // importing later. Formats we can't read directly go out uncompressed.
        Result<ByteBuffer> dds = std::unexpected(Error{});
        if (auto s = decoder.surfaces(data))
            dds = image::writeDds(*s);
        if (!dds) {
            auto img = decoder.decode(data);
            if (!img)
                return std::unexpected(img.error());
            auto s = image::encodeSurfaces(*img, image::PixelFormat::A8R8G8B8, 1);
            if (!s)
                return std::unexpected(s.error());
            dds = image::writeDds(*s);
            if (!dds)
                return std::unexpected(dds.error());
        }
        if (auto ok = writeFile(withExtension(base, ".dds"), *dds); !ok)
            return std::unexpected(ok.error());
        return 1;
    }

    auto img = decoder.decode(data);
    if (!img)
        return std::unexpected(img.error());
    QImage image = toQImage(*img);
    const char* qtFormat = "PNG";
    const char* extension = ".png";
    int quality = -1;
    if (format == ImageExport::Jpg) {
        image = image.convertToFormat(QImage::Format_RGB888); // JPEG has no transparency
        qtFormat = "JPG";
        extension = ".jpg";
        quality = 95;
    } else if (format == ImageExport::Bmp) {
        qtFormat = "BMP";
        extension = ".bmp";
    }
    const auto dest = withExtension(base, extension);
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (!image.save(toQString(dest), qtFormat, quality))
        return fail(ErrorCode::Io, "could not write {}", toQString(dest).toStdString());
    return 1;
}

/// Writes one converted item; returns how many files were written.
Result<int> exportOne(const Session& session, const ExtractItem& item, const std::filesystem::path& outputDir,
                      const ExportOptions& options, bool& converted)
{
    const VfsNode& node = *item.node;
    const std::string ext = extensionOf(node.name());
    const auto relative = sanitizeRelativePath(item.relativePath);
    converted = true;

    // Audio bank → folder of .wav files
    if (auto h = head(session, node); h) {
        if (const SoundBankFormat* format = bankFormat(session, node, *h)) {
            auto source = Vfs::source(node);
            if (!source)
                return std::unexpected(source.error());
            auto bank = format->open(*source);
            if (!bank)
                return std::unexpected(bank.error());
            const auto names = audio::soundFileNames((*bank)->sounds());
            const auto folder = outputDir / withExtension(relative, "");
            int written = 0;
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto clip = (*bank)->decode(i);
                if (!clip)
                    return std::unexpected(clip.error());
                if (auto ok = writeFile(folder / (names[i] + ".wav"), audio::encodeWav(*clip)); !ok)
                    return std::unexpected(ok.error());
                ++written;
            }
            return written;
        }
    }

    auto data = session.vfs.read(node);
    if (!data)
        return std::unexpected(data.error());

    // Model → .obj, and the textures stored in it → <name>_textures/*.png
    if (const MeshDecoder* decoder = session.content.findMeshDecoder(ext, *data)) {
        if (auto mesh = decoder->decodeWith(*data, session.vfs.decompileContext(node))) {
            const std::string obj = mesh::toObj(*mesh);
            if (auto ok = writeFile(outputDir / mesh::objPath(relative), asBytes(obj)); !ok)
                return std::unexpected(ok.error());
            int written = 1;
            if (!mesh->textures.empty()) {
                std::filesystem::path folder = outputDir / withExtension(relative, "");
                folder += "_textures";
                std::error_code ec;
                std::filesystem::create_directories(folder, ec);
                for (const Mesh::Texture& t : mesh->textures) {
                    const auto file = folder / sanitizeRelativePath(t.name + ".png");
                    if (!toQImage(t.image).save(toQString(file), "PNG"))
                        return fail(ErrorCode::Io, "could not write {}", toQString(file).toStdString());
                    ++written;
                }
            }
            return written;
        }
    }
    // Texture → picture
    if (const ImageDecoder* decoder = session.content.findImageDecoder(ext, *data))
        return exportTexture(*decoder, *data, ext, outputDir / relative, options.images, converted);

    // Decompiled → .txt (e.g. Blur's audio entities)
    if (const TextDecompiler* decompiler = session.content.findDecompiler(ext, *data)) {
        if (auto text = decompiler->decompileWith(*data, session.vfs.decompileContext(node))) {
            if (auto ok = writeFile(outputDir / withExtension(relative, ".txt"), asBytes(*text)); !ok)
                return std::unexpected(ok.error());
            return 1;
        }
    }

    // No converter: as is.
    converted = false;
    if (auto ok = writeFile(outputDir / relative, *data); !ok)
        return std::unexpected(ok.error());
    return 1;
}

/// Reads a .wav for a sound; a clear message when it isn't usable.
Result<AudioClip> readWav(const std::filesystem::path& file)
{
    auto bytes = readFile(file);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto clip = audio::decodeWav(*bytes);
    if (!clip)
        return fail(ErrorCode::InvalidFormat, "{} (only 8/16-bit PCM .wav files can be imported)", clip.error().message);
    if (clip->samples.empty())
        return fail(ErrorCode::InvalidFormat, "the file has no audio");
    return clip;
}

Result<BankImport> rebuildBank(const Session& session, const VfsNode& node,
                               const std::function<Result<std::vector<std::optional<AudioClip>>>(
                                   std::span<const SoundInfo>, BankImport&)>& choose)
{
    auto data = session.vfs.read(node);
    if (!data)
        return std::unexpected(data.error());
    const SoundBankFormat* format =
        bankFormat(session, node, ByteSpan(*data).first(std::min<std::size_t>(data->size(), 4096)));
    if (!format || !format->canRebuild())
        return fail(ErrorCode::Unsupported, "{} can't be rebuilt", node.name());
    auto bank = format->open(memorySource(*data));
    if (!bank)
        return std::unexpected(bank.error());

    BankImport result;
    auto replacements = choose((*bank)->sounds(), result);
    if (!replacements)
        return std::unexpected(replacements.error());
    if (result.replaced == 0)
        return fail(ErrorCode::NotFound, "no usable .wav file matches a sound in {}", node.name());
    auto rebuilt = format->rebuild(*data, *replacements);
    if (!rebuilt)
        return std::unexpected(rebuilt.error());
    result.data = std::move(*rebuilt);
    return result;
}

/// A texture's file, its decoder and its stored pixels.
struct LoadedTexture {
    ByteBuffer data;
    const ImageDecoder* decoder = nullptr;
    image::Surfaces surfaces;
};

Result<LoadedTexture> loadTexture(const Session& session, const VfsNode& node)
{
    LoadedTexture t;
    auto data = session.vfs.read(node);
    if (!data)
        return std::unexpected(data.error());
    t.data = std::move(*data);
    t.decoder = session.content.findImageDecoder(extensionOf(node.name()), t.data);
    if (!t.decoder || !t.decoder->canRebuild())
        return fail(ErrorCode::Unsupported, "pictures can't be imported into {} yet", node.name());
    auto s = t.decoder->surfaces(t.data);
    if (!s)
        return std::unexpected(s.error());
    t.surfaces = std::move(*s);
    return t;
}

} // namespace

QFuture<ExtractSummary> startExport(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                    std::filesystem::path outputDir, ExportOptions options)
{
    return QtConcurrent::run([session = std::move(session), items = std::move(items),
                              outputDir = std::move(outputDir), options](QPromise<ExtractSummary>& promise) {
        ExtractSummary summary;
        promise.setProgressRange(0, static_cast<int>(items.size()));
        int done = 0;
        for (const auto& item : items) {
            if (promise.isCanceled()) {
                summary.canceled = true;
                break;
            }
            promise.setProgressValueAndText(done, toQString(item.node->name()));
            bool converted = false;
            if (auto written = exportOne(*session, item, outputDir, options, converted)) {
                summary.written += *written;
                summary.converted += converted;
                summary.bytes += item.node->size();
            } else {
                ++summary.failed;
                if (summary.errors.size() < 20)
                    summary.errors << QStringLiteral("%1: %2").arg(toQString(item.relativePath),
                                                                   toQString(written.error().message));
            }
            ++done;
        }
        promise.setProgressValue(done);
        promise.addResult(summary);
    });
}

bool hasTextures(const Session& session, const std::vector<ExtractItem>& items)
{
    return std::ranges::any_of(items, [&](const ExtractItem& item) {
        const FileTypeInfo* type = session.content.fileTypes().find(extensionOf(item.node->name()));
        return type && type->category == FileCategory::Texture;
    });
}

ImportKind importKind(const Session& session, const VfsNode& node)
{
    if (!node.isFile() || !Vfs::canReplace(node))
        return ImportKind::None;
    const auto h = head(session, node);
    if (!h)
        return ImportKind::None;
    if (const SoundBankFormat* format = bankFormat(session, node, *h); format && format->canRebuild())
        return ImportKind::SoundBank;
    if (const ImageDecoder* decoder = session.content.findImageDecoder(extensionOf(node.name()), *h);
        decoder && decoder->canRebuild())
        return ImportKind::Texture;
    if (textureDictionary(session, node))
        return ImportKind::TextureDictionary;
    return ImportKind::None;
}

Result<BankImport> planBankImport(const Session& session, const VfsNode& node, const std::filesystem::path& folder)
{
    return rebuildBank(session, node, [&](std::span<const SoundInfo> sounds, BankImport& result)
                                          -> Result<std::vector<std::optional<AudioClip>>> {
        // file name (lower case, no extension) → sound index, as Export names them
        std::map<std::string, std::size_t> byName;
        const auto names = audio::soundFileNames(sounds);
        for (std::size_t i = 0; i < names.size(); ++i)
            byName[lower(names[i])] = i;

        std::vector<std::optional<AudioClip>> replacements(sounds.size());
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (!entry.is_regular_file() || lower(entry.path().extension().string()) != ".wav")
                continue;
            const auto it = byName.find(lower(toQString(entry.path().stem()).toStdString()));
            const QString file = toQString(entry.path().filename());
            if (it == byName.end()) {
                result.unmatched << file;
                continue;
            }
            if (auto clip = readWav(entry.path())) {
                replacements[it->second] = std::move(*clip);
                ++result.replaced;
            } else {
                result.failed << QStringLiteral("%1: %2").arg(file, toQString(clip.error().message));
            }
        }
        if (ec)
            return fail(ErrorCode::Io, "can't read the folder {}: {}", toQString(folder).toStdString(), ec.message());
        result.kept = static_cast<int>(sounds.size()) - result.replaced;
        return replacements;
    });
}

Result<BankImport> planSoundReplace(const Session& session, const VfsNode& node, std::size_t index,
                                    const std::filesystem::path& file)
{
    return rebuildBank(session, node, [&](std::span<const SoundInfo> sounds, BankImport& result)
                                          -> Result<std::vector<std::optional<AudioClip>>> {
        if (index >= sounds.size())
            return fail(ErrorCode::NotFound, "sound {} isn't in this bank", index + 1);
        auto clip = readWav(file);
        if (!clip)
            return std::unexpected(clip.error());
        std::vector<std::optional<AudioClip>> replacements(sounds.size());
        replacements[index] = std::move(*clip);
        result.replaced = 1;
        result.kept = static_cast<int>(sounds.size()) - 1;
        return replacements;
    });
}

Result<TextureTarget> textureTarget(const Session& session, const VfsNode& node)
{
    auto t = loadTexture(session, node);
    if (!t)
        return std::unexpected(t.error());
    TextureTarget target;
    target.format = t->surfaces.format;
    target.width = t->surfaces.width;
    target.height = t->surfaces.height;
    target.mipCount = t->surfaces.mipCount;
    target.formats = t->decoder->importFormats(t->data);
    if (target.formats.empty())
        return fail(ErrorCode::Unsupported, "{}",
                    tr("Only flat textures can be imported so far (not cube maps or volume textures).").toStdString());
    return target;
}

Result<Picture> loadPicture(const std::filesystem::path& file)
{
    Picture picture;
    if (lower(file.extension().string()) == ".dds") {
        auto bytes = readFile(file);
        if (!bytes)
            return std::unexpected(bytes.error());
        auto s = image::readDds(*bytes);
        if (!s)
            return std::unexpected(s.error());
        auto img = image::decodeSurface(s->data, s->format, s->width, s->height);
        if (!img)
            return std::unexpected(img.error());
        picture.image = std::move(*img);
        picture.dds = std::move(*s);
    } else {
        QImageReader reader(toQString(file));
        reader.setAutoTransform(true);
        const QImage loaded = reader.read();
        if (loaded.isNull())
            return fail(ErrorCode::InvalidFormat, "{}", reader.errorString().toStdString());
        const QImage rgba = loaded.convertToFormat(QImage::Format_RGBA8888);
        picture.image.width = static_cast<std::uint32_t>(rgba.width());
        picture.image.height = static_cast<std::uint32_t>(rgba.height());
        picture.image.rgba.resize(static_cast<std::size_t>(rgba.width()) * rgba.height() * 4);
        const auto row = static_cast<std::size_t>(rgba.width()) * 4;
        for (int y = 0; y < rgba.height(); ++y)
            std::memcpy(picture.image.rgba.data() + row * static_cast<std::size_t>(y), rgba.constScanLine(y), row);
    }
    for (std::size_t i = 3; i < picture.image.rgba.size(); i += 4) {
        if (picture.image.rgba[i] != 255) {
            picture.translucent = true;
            break;
        }
    }
    return picture;
}

Result<ByteBuffer> buildTextureImport(const Session& session, const VfsNode& node, const Picture& picture,
                                      image::PixelFormat format, QStringList& notes)
{
    auto t = loadTexture(session, node);
    if (!t)
        return std::unexpected(t.error());
    if (!std::ranges::contains(t->decoder->importFormats(t->data), format))
        return fail(ErrorCode::Unsupported, "{} can't be stored as {}", node.name(), image::pixelFormatName(format));

    const std::uint32_t w = picture.image.width, h = picture.image.height;
    const QString size = QStringLiteral("%1×%2").arg(w).arg(h);
    if (w > 8192 || h > 8192)
        return fail(ErrorCode::Unsupported, "{}",
                    tr("The picture is %1; the largest texture the game can use is 8192×8192.").arg(size).toStdString());
    if (image::isCompressed(format) && (w % 4 || h % 4))
        return fail(ErrorCode::Unsupported, "{}",
                    tr("DXT textures need a width and height that divide by 4 — the picture is %1. "
                       "Resize it (to a power of two such as 512 or 1024) and import again.")
                        .arg(size)
                        .toStdString());

    const image::Surfaces& original = t->surfaces;
    if (w != original.width || h != original.height)
        notes << tr("Size changes from %1×%2 to %3.").arg(original.width).arg(original.height).arg(size);
    if (!std::has_single_bit(w) || !std::has_single_bit(h))
        notes << tr("%1 isn't a power of two (256, 512, 1024…); the game may not show it correctly.").arg(size);
    if (format == image::PixelFormat::BC1 && picture.translucent)
        notes << tr("DXT1 keeps transparency only as on/off: pixels under 50% become fully see-through.");

    // Mip-maps (smaller copies for distant surfaces) the way the original has them.
    const std::uint32_t mips = original.mipCount > 1 ? image::fullMipCount(w, h) : 1;
    image::Surfaces surfaces;
    if (picture.dds && picture.dds->format == format && picture.dds->layers == 1 && picture.dds->mipCount >= mips) {
        // Already in the right format: the .dds's own pixels and mip-maps, not re-compressed.
        surfaces = *picture.dds;
        surfaces.mipCount = mips;
        surfaces.data.resize(image::chainSize(format, w, h, mips));
        notes << tr("The .dds is already %1, so its pixels are used as they are.")
                     .arg(toQString(image::pixelFormatName(format)));
    } else {
        auto encoded = image::encodeSurfaces(picture.image, format, mips);
        if (!encoded)
            return std::unexpected(encoded.error());
        surfaces = std::move(*encoded);
        notes << (mips > 1 ? tr("Converted to %1 with %2 mip-map levels.")
                                 .arg(toQString(image::pixelFormatName(format)))
                                 .arg(mips)
                           : tr("Converted to %1 (no mip-maps, like the original).")
                                 .arg(toQString(image::pixelFormatName(format))));
    }
    return t->decoder->rebuild(t->data, surfaces);
}

// ---- Texture dictionaries ---------------------------------------------------------------

const TextureDictionaryFormat* textureDictionary(const Session& session, const VfsNode& node)
{
    if (!node.isFile())
        return nullptr;
    const auto h = head(session, node);
    return h ? session.content.findTextureDictionary(extensionOf(node.name()), *h) : nullptr;
}

namespace {

struct LoadedDictionary {
    const TextureDictionaryFormat* format = nullptr;
    ByteBuffer data;
    std::vector<DictionaryTexture> textures;
};

Result<LoadedDictionary> loadDictionary(const Session& session, const VfsNode& node)
{
    LoadedDictionary d;
    d.format = textureDictionary(session, node);
    if (!d.format)
        return fail(ErrorCode::Unsupported, "{} holds no textures that can be replaced", node.name());
    auto data = session.vfs.read(node);
    if (!data)
        return std::unexpected(data.error());
    d.data = std::move(*data);
    auto list = d.format->list(d.data);
    if (!list)
        return std::unexpected(list.error());
    d.textures = std::move(*list);
    if (d.textures.empty())
        return fail(ErrorCode::NotFound, "{} holds no textures", node.name());
    return d;
}

/// `picture` as texture `t` will store it: its size, format and mip levels.
Result<image::Surfaces> fitPicture(const Picture& picture, const DictionaryTexture& t, QStringList& notes)
{
    const QString name = toQString(t.name);
    if (picture.dds && picture.dds->format == t.format && picture.dds->width == t.width
        && picture.dds->height == t.height && picture.dds->layers == 1 && picture.dds->mipCount >= t.mipCount) {
        // Already right: the .dds's own pixels, not re-compressed.
        image::Surfaces s = *picture.dds;
        s.mipCount = t.mipCount;
        s.data.resize(image::chainSize(t.format, t.width, t.height, t.mipCount));
        notes << tr("%1: the .dds is already %2, used as it is.").arg(name, toQString(image::pixelFormatName(t.format)));
        return s;
    }
    if (!image::canEncode(t.format))
        return fail(ErrorCode::Unsupported, "{} is stored as {}, which can't be written yet", t.name,
                    image::pixelFormatName(t.format));
    Image img = picture.image;
    if (img.width != t.width || img.height != t.height) {
        const QImage scaled = toQImage(img).scaled(static_cast<int>(t.width), static_cast<int>(t.height),
                                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                  .convertToFormat(QImage::Format_RGBA8888);
        notes << tr("%1: the picture (%2×%3) is scaled to the texture's %4×%5.")
                     .arg(name)
                     .arg(img.width)
                     .arg(img.height)
                     .arg(t.width)
                     .arg(t.height);
        img.width = t.width;
        img.height = t.height;
        img.rgba.assign(scaled.constBits(), scaled.constBits() + scaled.sizeInBytes());
    }
    if (t.format == image::PixelFormat::BC1 && picture.translucent)
        notes << tr("%1: DXT1 keeps transparency only as on/off.").arg(name);
    return image::encodeSurfaces(img, t.format, t.mipCount);
}

} // namespace

Result<DictionaryImport> planDictionaryReplace(const Session& session, const VfsNode& node, std::size_t index,
                                               const Picture& picture)
{
    auto d = loadDictionary(session, node);
    if (!d)
        return std::unexpected(d.error());
    if (index >= d->textures.size())
        return fail(ErrorCode::NotFound, "no texture #{} in {}", index + 1, node.name());
    DictionaryImport result;
    auto surfaces = fitPicture(picture, d->textures[index], result.notes);
    if (!surfaces)
        return std::unexpected(surfaces.error());
    auto data = d->format->replace(d->data, index, *surfaces);
    if (!data)
        return std::unexpected(data.error());
    result.data = std::move(*data);
    result.replaced = 1;
    result.kept = static_cast<int>(d->textures.size()) - 1;
    return result;
}

std::vector<std::filesystem::path> picturesIn(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> pictures;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const std::string ext = lower(entry.path().extension().string());
        if (entry.is_regular_file() && (ext == ".png" || ext == ".dds" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp"))
            pictures.push_back(entry.path());
    }
    std::ranges::sort(pictures);
    return pictures;
}

Result<DictionaryImport> planDictionaryImport(const Session& session, const VfsNode& node,
                                              const std::vector<std::filesystem::path>& pictures)
{
    auto d = loadDictionary(session, node);
    if (!d)
        return std::unexpected(d.error());
    std::map<std::string, std::size_t> byName; // lower-case texture name -> index
    for (std::size_t i = 0; i < d->textures.size(); ++i)
        byName.try_emplace(lower(d->textures[i].name), i);

    DictionaryImport result;
    result.data = std::move(d->data);
    std::vector<bool> done(d->textures.size(), false);
    for (const std::filesystem::path& path : pictures) {
        const QString file = toQString(path.filename());
        const auto it = byName.find(lower(toQString(path.stem()).toStdString()));
        if (it == byName.end()) {
            result.unmatched << file;
            continue;
        }
        if (done[it->second]) {
            result.failed << tr("%1: another picture for this texture was used").arg(file);
            continue;
        }
        auto picture = loadPicture(path);
        if (!picture) {
            result.failed << QStringLiteral("%1: %2").arg(file, toQString(picture.error().message));
            continue;
        }
        auto surfaces = fitPicture(*picture, d->textures[it->second], result.notes);
        if (!surfaces) {
            result.failed << QStringLiteral("%1: %2").arg(file, toQString(surfaces.error().message));
            continue;
        }
        auto data = d->format->replace(result.data, it->second, *surfaces);
        if (!data) {
            result.failed << QStringLiteral("%1: %2").arg(file, toQString(data.error().message));
            continue;
        }
        result.data = std::move(*data);
        done[it->second] = true;
        ++result.replaced;
    }
    result.kept = static_cast<int>(std::ranges::count(done, false));
    if (result.replaced == 0 && result.failed.isEmpty())
        return fail(ErrorCode::NotFound, "{}",
                    tr("No picture is named after a texture of %1 (e.g. %2.png).")
                        .arg(toQString(node.name()), toQString(d->textures.front().name))
                        .toStdString());
    return result;
}

} // namespace viewer::app
