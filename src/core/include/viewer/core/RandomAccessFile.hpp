#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace viewer {

/// A read-only file that supports positional reads from many threads at once.
///
/// Archives are opened once and then read concurrently (preview + background
/// extraction). Positional reads (ReadFile with an OVERLAPPED offset on
/// Windows, pread elsewhere) avoid a shared seek pointer, so no locking is needed.
class RandomAccessFile {
public:
    static Result<std::unique_ptr<RandomAccessFile>> open(const std::filesystem::path& path);

    ~RandomAccessFile();
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return m_size; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    /// Fills `out` completely from `offset`, or fails.
    Result<void> readAt(std::uint64_t offset, std::span<std::byte> out) const;

    /// Reads `count` bytes from `offset` into a new buffer.
    Result<ByteBuffer> read(std::uint64_t offset, std::size_t count) const;

private:
    RandomAccessFile() = default;

    std::filesystem::path m_path;
    std::uint64_t m_size = 0;
#ifdef _WIN32
    void* m_handle = nullptr;
#else
    int m_fd = -1;
#endif
};

/// Reads a whole file (or at most `limit` bytes of it) into memory.
Result<ByteBuffer> readFile(const std::filesystem::path& path, std::uint64_t limit = UINT64_MAX);

/// Writes a buffer to disk, creating parent directories as needed.
Result<void> writeFile(const std::filesystem::path& path, ByteSpan data);

/// Makes a file writable for the lifetime of the object, then restores its
/// read-only flag. Game installs often mark their archives read-only.
class ScopedWritable {
public:
    explicit ScopedWritable(std::filesystem::path path);
    ~ScopedWritable();
    ScopedWritable(const ScopedWritable&) = delete;
    ScopedWritable& operator=(const ScopedWritable&) = delete;

    /// Error if the file could not be made writable.
    [[nodiscard]] const Result<void>& status() const noexcept { return m_status; }

private:
    std::filesystem::path m_path;
    bool m_wasReadOnly = false;
    Result<void> m_status;
};

/// Where the untouched original of `file` is kept: "<file>.bak" beside it.
[[nodiscard]] std::filesystem::path backupPathFor(const std::filesystem::path& file);

/// Copies `file` to its backup path unless a backup already exists (an
/// existing backup is the true original and is never overwritten).
/// Returns the backup path.
Result<std::filesystem::path> ensureBackup(const std::filesystem::path& file);

/// Copies the backup back over `file`. The file must not be open elsewhere.
Result<void> restoreBackup(const std::filesystem::path& file);

} // namespace viewer
