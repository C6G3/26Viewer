#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace viewer {

class RandomAccessFile;

/// Random access to a file's bytes without loading all of it — used for big
/// containers (audio banks of hundreds of MB) where only a header and one
/// item are needed at a time. Implementations must be thread-safe.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    /// Reads exactly `count` bytes at `offset`, or fails.
    [[nodiscard]] virtual Result<ByteBuffer> read(std::uint64_t offset, std::size_t count) const = 0;
};

/// A source over bytes already in memory.
[[nodiscard]] std::shared_ptr<const ByteSource> memorySource(ByteBuffer bytes);

/// A window [offset, offset + size) of an open file. The file must outlive the source.
[[nodiscard]] std::shared_ptr<const ByteSource> fileSliceSource(const RandomAccessFile& file, std::uint64_t offset,
                                                                std::uint64_t size);

/// A whole file on disk (the source keeps it open).
[[nodiscard]] Result<std::shared_ptr<const ByteSource>> fileSource(const std::filesystem::path& path);

} // namespace viewer
