#pragma once

#include "viewer/core/Animation.hpp"
#include "viewer/core/Archive.hpp"
#include "viewer/core/Audio.hpp"
#include "viewer/core/FileTypes.hpp"
#include "viewer/core/Image.hpp"
#include "viewer/core/Mesh.hpp"
#include "viewer/core/Text.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// A node in a parsed file layout, shown in the "Structure" tab.
struct StructureNode {
    std::string label;          ///< "Texture", "Header", "MdlDat"...
    std::string detail;         ///< Free text summary.
    std::uint64_t offset = 0;   ///< Byte offset of the node within the file.
    std::uint64_t size = 0;
    std::vector<StructureNode> children;
};

/// Converts a file's bytes into an Image (textures, DDS, game-specific images).
class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecode(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<Image> decode(ByteSpan data) const = 0;

    /// The stored pixels, unconverted (used to export a lossless .dds).
    [[nodiscard]] virtual Result<image::Surfaces> surfaces(ByteSpan data) const
    {
        (void)data;
        return std::unexpected(Error{ErrorCode::Unsupported, "this texture's pixels can't be read directly"});
    }
    /// Whether rebuild() is supported (pictures can be imported into this type).
    [[nodiscard]] virtual bool canRebuild() const noexcept { return false; }
    /// Pixel formats a picture imported into this texture can be stored in
    /// (the game's choices). Empty: importing isn't supported.
    [[nodiscard]] virtual std::vector<image::PixelFormat> importFormats(ByteSpan data) const
    {
        (void)data;
        return {};
    }
    /// A copy of the texture file `original` holding `surfaces` instead of its
    /// pixels; everything else in the file stays as it was.
    [[nodiscard]] virtual Result<ByteBuffer> rebuild(ByteSpan original, const image::Surfaces& surfaces) const
    {
        (void)original;
        (void)surfaces;
        return std::unexpected(Error{ErrorCode::Unsupported, "this texture format can't be written yet"});
    }
};

/// One texture inside a texture dictionary.
struct DictionaryTexture {
    std::string name;
    image::PixelFormat format = image::PixelFormat::BC1;
    std::uint32_t width = 0;   ///< Most detailed level.
    std::uint32_t height = 0;
    std::uint32_t mipCount = 1;
};

/// A file holding several textures — a "texture dictionary" (Blur keeps a
/// model's textures inside the model). Handled like a sound bank: list them,
/// read one's pixels, and write a copy of the file with some replaced.
class TextureDictionaryFormat {
public:
    virtual ~TextureDictionaryFormat() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canRead(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<std::vector<DictionaryTexture>> list(ByteSpan data) const = 0;
    /// Texture `index`'s stored pixels, unconverted (every mip level kept).
    [[nodiscard]] virtual Result<image::Surfaces> surfaces(ByteSpan data, std::size_t index) const = 0;
    /// A copy of `data` with texture `index` holding `surfaces`, which must have
    /// the texture's own size, pixel format and mip count (everything else in
    /// the file stays byte for byte as it was).
    [[nodiscard]] virtual Result<ByteBuffer> replace(ByteSpan data, std::size_t index,
                                                     const image::Surfaces& surfaces) const = 0;
};

/// Breaks a file into a tree of labelled regions (chunk formats, headers...).
class StructureParser {
public:
    virtual ~StructureParser() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canParse(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<StructureNode> parse(ByteSpan data) const = 0;
};

/// Turns a binary file into readable text ("decompiles" it): shown read-only
/// in the text view and written as .txt by Export.
class TextDecompiler {
public:
    virtual ~TextDecompiler() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canDecompile(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<std::string> decompile(ByteSpan data) const = 0;
    /// Colour coding for the text.
    [[nodiscard]] virtual TextSyntax syntax() const noexcept { return TextSyntax::Plain; }
};

/// Everything a session knows how to open, decode and describe.
///
/// Built-in (game-agnostic) handlers are registered first by
/// registerBuiltinContent(); the active game then adds its own. Lookups search
/// newest-first, so a game can override a global handler for its own files.
class ContentRegistry {
public:
    FileTypeRegistry& fileTypes() noexcept { return m_fileTypes; }
    [[nodiscard]] const FileTypeRegistry& fileTypes() const noexcept { return m_fileTypes; }

    void addArchiveFormat(std::shared_ptr<const ArchiveFormat> format);
    void addImageDecoder(std::shared_ptr<const ImageDecoder> decoder);
    void addStructureParser(std::shared_ptr<const StructureParser> parser);
    void addMeshDecoder(std::shared_ptr<const MeshDecoder> decoder);
    void addSoundBankFormat(std::shared_ptr<const SoundBankFormat> format);
    void addDecompiler(std::shared_ptr<const TextDecompiler> decompiler);
    void addTextureDictionary(std::shared_ptr<const TextureDictionaryFormat> format);
    void addSkeletonDecoder(std::shared_ptr<const SkeletonDecoder> decoder);
    void addAnimationDecoder(std::shared_ptr<const AnimationDecoder> decoder);

    /// Finds a format able to open `path` (extension filter, then header probe).
    [[nodiscard]] const ArchiveFormat* findArchiveFormat(const std::filesystem::path& path) const;
    [[nodiscard]] const ImageDecoder* findImageDecoder(std::string_view extension, ByteSpan data) const;
    [[nodiscard]] const StructureParser* findStructureParser(std::string_view extension, ByteSpan data) const;
    [[nodiscard]] const MeshDecoder* findMeshDecoder(std::string_view extension, ByteSpan data) const;
    /// `header`: the first bytes of the file (up to 4 KiB).
    [[nodiscard]] const SoundBankFormat* findSoundBankFormat(std::string_view extension, ByteSpan header) const;
    [[nodiscard]] const TextDecompiler* findDecompiler(std::string_view extension, ByteSpan data) const;
    [[nodiscard]] const TextureDictionaryFormat* findTextureDictionary(std::string_view extension, ByteSpan data) const;
    [[nodiscard]] const SkeletonDecoder* findSkeletonDecoder(std::string_view extension, ByteSpan data) const;
    [[nodiscard]] const AnimationDecoder* findAnimationDecoder(std::string_view extension, ByteSpan data) const;

    [[nodiscard]] const auto& archiveFormats() const noexcept { return m_archiveFormats; }

private:
    FileTypeRegistry m_fileTypes;
    std::vector<std::shared_ptr<const ArchiveFormat>> m_archiveFormats;
    std::vector<std::shared_ptr<const ImageDecoder>> m_imageDecoders;
    std::vector<std::shared_ptr<const StructureParser>> m_structureParsers;
    std::vector<std::shared_ptr<const MeshDecoder>> m_meshDecoders;
    std::vector<std::shared_ptr<const SoundBankFormat>> m_soundBankFormats;
    std::vector<std::shared_ptr<const TextDecompiler>> m_decompilers;
    std::vector<std::shared_ptr<const TextureDictionaryFormat>> m_textureDictionaries;
    std::vector<std::shared_ptr<const SkeletonDecoder>> m_skeletonDecoders;
    std::vector<std::shared_ptr<const AnimationDecoder>> m_animationDecoders;
};

/// Registers handlers shared by all games: common file types, DDS decoding.
void registerBuiltinContent(ContentRegistry& registry);

/// Heuristic: does this look like human-readable text (UTF-8/ASCII/UTF-16 with BOM)?
[[nodiscard]] bool looksLikeText(ByteSpan data) noexcept;

} // namespace viewer
