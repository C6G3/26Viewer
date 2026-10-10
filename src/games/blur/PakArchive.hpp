#pragma once

#include "viewer/core/Archive.hpp"
#include "viewer/core/RandomAccessFile.hpp"

#include <array>
#include <memory>
#include <vector>

namespace viewer::games::blur {

/// Blur's "2KAP" package format (cache/*.pak). See docs/formats/blur.md.
class PakArchive final : public Archive {
public:
    static Result<std::unique_ptr<PakArchive>> open(const std::filesystem::path& path);

    std::string_view formatName() const noexcept override { return "Blur PAK"; }
    const std::filesystem::path& path() const noexcept override { return m_file->path(); }
    std::span<const ArchiveEntry> entries() const noexcept override { return m_entries; }
    Result<ByteBuffer> read(std::size_t index) const override;
    /// Uncompressed entries (e.g. streaming audio banks) are read in place.
    Result<std::shared_ptr<const ByteSource>> openEntry(std::size_t index) const override;
    PropertyList describeEntry(std::size_t index) const override;
    PropertyList describe() const override;

    bool canReplace() const noexcept override { return true; }
    /// Appends the newly encoded data at the end of the archive and re-points
    /// the entry's record at it (other entries are untouched).
    Result<ReplaceReport> replace(std::size_t index, ByteSpan data) const override;

    /// Blur's name hash: CRC-32 without the final inversion over the lower-case
    /// in-archive path with backslashes, e.g. `\models\cars\a.model`. The file
    /// table is sorted by it (the game binary-searches it).
    static std::uint32_t nameHash(std::string_view archivePath) noexcept;
    /// Blur's content hash: the 32-bit sum of `~crc32(block)` over every
    /// 32 KiB block of the uncompressed data.
    static std::uint32_t contentHash(ByteSpan data) noexcept;

private:
    /// Format-specific data kept alongside each public ArchiveEntry.
    struct Slot {
        std::uint64_t entryOffset = 0; ///< File offset of this entry's table record.
        std::uint64_t dataOffset = 0;
        std::uint32_t storedSize = 0; ///< v2: stored size; v1: zlib size after the u32 prefix.
        std::uint32_t hash = 0;
        std::uint32_t nameHash = 0;
        std::array<std::byte, 20> digest{};
        bool hasDigest = false;
    };

    enum class Storage { Stored, Zlib, ChunkedDeflate };

    PakArchive() = default;
    Result<Storage> detectStorage(const Slot& slot, std::uint64_t size) const;
    Result<ByteBuffer> readChunked(ByteSpan raw, std::size_t size) const;
    Result<ByteBuffer> encodeLike(const Slot& slot, std::uint64_t originalSize, ByteSpan data,
                                  std::string& storageName) const;

    std::unique_ptr<RandomAccessFile> m_file;
    std::vector<ArchiveEntry> m_entries;
    std::vector<Slot> m_slots;

    std::uint32_t m_version = 0;
    std::uint32_t m_alignment = 0;
    std::uint16_t m_flags = 0;
    std::uint32_t m_entryStride = 0;
    std::uint32_t m_directoryCount = 0;
    std::uint32_t m_nameTableSize = 0;
};

/// Registers PakArchive with the content registry.
class PakFormat final : public ArchiveFormat {
public:
    std::string_view name() const noexcept override { return "Blur PAK"; }
    std::vector<std::string> extensions() const override { return {"pak"}; }
    bool probe(ByteSpan header, const std::filesystem::path& path) const override;
    Result<std::unique_ptr<Archive>> open(const std::filesystem::path& path) const override;
};

} // namespace viewer::games::blur
