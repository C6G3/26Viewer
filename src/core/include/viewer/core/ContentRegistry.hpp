#pragma once

#include "viewer/core/Archive.hpp"
#include "viewer/core/FileTypes.hpp"
#include "viewer/core/Image.hpp"

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
};

/// Breaks a file into a tree of labelled regions (chunk formats, headers...).
class StructureParser {
public:
    virtual ~StructureParser() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool canParse(std::string_view extension, ByteSpan data) const = 0;
    [[nodiscard]] virtual Result<StructureNode> parse(ByteSpan data) const = 0;
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

    /// Finds a format able to open `path` (extension filter, then header probe).
    [[nodiscard]] const ArchiveFormat* findArchiveFormat(const std::filesystem::path& path) const;
    [[nodiscard]] const ImageDecoder* findImageDecoder(std::string_view extension, ByteSpan data) const;
    [[nodiscard]] const StructureParser* findStructureParser(std::string_view extension, ByteSpan data) const;

    [[nodiscard]] const auto& archiveFormats() const noexcept { return m_archiveFormats; }

private:
    FileTypeRegistry m_fileTypes;
    std::vector<std::shared_ptr<const ArchiveFormat>> m_archiveFormats;
    std::vector<std::shared_ptr<const ImageDecoder>> m_imageDecoders;
    std::vector<std::shared_ptr<const StructureParser>> m_structureParsers;
};

/// Registers handlers shared by all games: common file types, DDS decoding.
void registerBuiltinContent(ContentRegistry& registry);

/// Heuristic: does this look like human-readable text (UTF-8/ASCII/UTF-16 with BOM)?
[[nodiscard]] bool looksLikeText(ByteSpan data) noexcept;

} // namespace viewer
