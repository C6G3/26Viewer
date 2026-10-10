#pragma once

#include "Extractor.hpp"

#include "viewer/core/Image.hpp"

#include <optional>

namespace viewer::app {

// Export / Import — the "converted" counterparts of Extract / Replace:
//
//   Extract  copies files out exactly as the game stores them.
//   Export   converts what it can into files any program opens:
//              audio banks → a folder of .wav files (one per sound)
//              models      → .obj
//              textures    → .png / .dds / .jpg / .bmp (the user's choice)
//            Anything without a converter is written as is.
//   Replace  puts a raw file back (same type as the original).
//   Import   converts back: a folder of .wav files → the bank, a picture →
//            the texture (re-compressed in the game's pixel format).

/// The picture format textures are exported as.
enum class ImageExport { Png, Dds, Jpg, Bmp };

struct ExportOptions {
    ImageExport images = ImageExport::Png;
};

/// Same progress / cancel behaviour as startExtraction.
QFuture<ExtractSummary> startExport(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                    std::filesystem::path outputDir, ExportOptions options);

/// Whether any of `items` is a texture (Export then asks which picture format).
[[nodiscard]] bool hasTextures(const Session& session, const std::vector<ExtractItem>& items);

/// What Import does for `node`.
enum class ImportKind { None, SoundBank, Texture };
/// Whether `node` can be rebuilt from exported files (Import), and how.
[[nodiscard]] ImportKind importKind(const Session& session, const VfsNode& node);

// ---- Audio banks ----

/// Result of rebuilding a bank from .wav files (see planBankImport).
struct BankImport {
    ByteBuffer data;        ///< The rebuilt bank.
    int replaced = 0;
    int kept = 0;           ///< Sounds with no matching file (unchanged).
    QStringList unmatched;  ///< .wav files that match no sound.
    QStringList failed;     ///< Files that couldn't be read ("name: reason").
};

/// Rebuilds the bank `node` with the .wav files in `folder` (matched to sounds
/// by the names Export gives them; other sounds stay as they are). Slow for big
/// banks (re-encoding): run it on a worker thread.
Result<BankImport> planBankImport(const Session& session, const VfsNode& node, const std::filesystem::path& folder);

/// Rebuilds the bank `node` with sound `index` replaced by the .wav `file`.
Result<BankImport> planSoundReplace(const Session& session, const VfsNode& node, std::size_t index,
                                    const std::filesystem::path& file);

// ---- Textures ----

/// What a texture holds now and the pixel formats the game accepts for it.
struct TextureTarget {
    image::PixelFormat format = image::PixelFormat::BC1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 1;
    std::vector<image::PixelFormat> formats; ///< Choices for the imported picture.
};
Result<TextureTarget> textureTarget(const Session& session, const VfsNode& node);

/// A picture to import: PNG, JPG, BMP or DDS.
struct Picture {
    Image image;                          ///< Its pixels (top mip of a .dds).
    std::optional<image::Surfaces> dds;   ///< A .dds file's own stored pixels.
    bool translucent = false;             ///< Some pixels aren't fully opaque.
};
Result<Picture> loadPicture(const std::filesystem::path& file);

/// The texture file `node` with its pixels replaced by `picture`, stored as
/// `format` (mip-maps made the way the original has them). Notes for the user
/// (size changes, lost transparency...) go into `notes`. DXT compression of
/// big pictures takes a moment: run it on a worker thread.
Result<ByteBuffer> buildTextureImport(const Session& session, const VfsNode& node, const Picture& picture,
                                      image::PixelFormat format, QStringList& notes);

} // namespace viewer::app
