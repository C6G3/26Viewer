#pragma once

#include "viewer/core/ByteSource.hpp"
#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// A name/value pair shown in the UI's "Properties" panel.
struct Property {
    std::string name;
    std::string value;
};
using PropertyList = std::vector<Property>;

/// One file stored in an archive.
struct ArchiveEntry {
    /// Normalised path: forward slashes, no leading slash ("models/cars/a.model").
    std::string path;
    std::uint64_t size = 0;       ///< Uncompressed size in bytes.
    std::uint64_t packedSize = 0; ///< Size as stored in the archive.
    std::optional<std::chrono::system_clock::time_point> modified;
};

/// Outcome of replacing a file's contents on disk.
struct ReplaceReport {
    std::string storage;           ///< How the new data was written ("chunked deflate", ...).
    std::uint64_t storedSize = 0;  ///< Bytes written for the entry.
    std::filesystem::path backup;  ///< Existing backup of the original file (empty if none).
    std::vector<std::string> warnings;
};

/// An opened archive. Implementations must allow read() from several threads
/// at once (the UI previews while a background job extracts).
class Archive {
public:
    virtual ~Archive() = default;

    [[nodiscard]] virtual std::string_view formatName() const noexcept = 0;
    [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
    [[nodiscard]] virtual std::span<const ArchiveEntry> entries() const noexcept = 0;

    /// Returns the fully decompressed contents of entry `index`.
    [[nodiscard]] virtual Result<ByteBuffer> read(std::size_t index) const = 0;

    /// Random access to entry `index` without necessarily loading all of it.
    /// The default reads the whole entry once; formats override this for
    /// entries stored uncompressed. The source must not outlive the archive.
    [[nodiscard]] virtual Result<std::shared_ptr<const ByteSource>> openEntry(std::size_t index) const;

    /// Format-specific details about one entry (compression, hashes, ...).
    [[nodiscard]] virtual PropertyList describeEntry(std::size_t /*index*/) const { return {}; }
    /// Format-specific details about the archive as a whole.
    [[nodiscard]] virtual PropertyList describe() const { return {}; }

    /// True if this format implements replace().
    [[nodiscard]] virtual bool canReplace() const noexcept { return false; }

    /// Writes new contents for entry `index` into the archive file on disk,
    /// keeping the entry's name. Encodes the data the same way the original
    /// was stored. This object is NOT updated: re-open the archive afterwards.
    /// Callers are responsible for backups (see Vfs::replace).
    [[nodiscard]] virtual Result<ReplaceReport> replace(std::size_t /*index*/, ByteSpan /*data*/) const
    {
        return fail(ErrorCode::Unsupported, "{} archives cannot be modified yet", formatName());
    }
};

/// Recognises and opens one archive format. Registered by game modules (or
/// globally for formats shared by many games).
class ArchiveFormat {
public:
    virtual ~ArchiveFormat() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /// Lowercase extensions without the dot, e.g. {"pak"}.
    [[nodiscard]] virtual std::vector<std::string> extensions() const = 0;
    /// Cheap check on the first bytes of a file (up to 4 KiB are provided).
    [[nodiscard]] virtual bool probe(ByteSpan header, const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Archive>> open(const std::filesystem::path& path) const = 0;
};

/// Turns "\\Models\\a.model" or "/x//y" into "Models/a.model" / "x/y".
[[nodiscard]] std::string normalizeArchivePath(std::string_view raw);

/// Converts a Windows FILETIME (100ns ticks since 1601) to a time_point.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> fromFileTime(std::uint64_t ticks) noexcept;
/// The reverse of fromFileTime().
[[nodiscard]] std::uint64_t toFileTime(std::chrono::system_clock::time_point time) noexcept;

} // namespace viewer
