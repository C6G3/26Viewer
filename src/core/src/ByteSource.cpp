#include "viewer/core/ByteSource.hpp"
#include "viewer/core/RandomAccessFile.hpp"

namespace viewer {

namespace {

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(ByteBuffer bytes) : m_bytes(std::move(bytes)) {}
    std::uint64_t size() const noexcept override { return m_bytes.size(); }
    Result<ByteBuffer> read(std::uint64_t offset, std::size_t count) const override
    {
        if (offset > m_bytes.size() || count > m_bytes.size() - offset)
            return fail(ErrorCode::OutOfBounds, "read of {} bytes at {} is past the end ({} bytes)", count, offset,
                        m_bytes.size());
        const auto first = m_bytes.begin() + static_cast<std::ptrdiff_t>(offset);
        return ByteBuffer(first, first + static_cast<std::ptrdiff_t>(count));
    }

private:
    ByteBuffer m_bytes;
};

class SliceSource : public ByteSource {
public:
    SliceSource(const RandomAccessFile& file, std::uint64_t offset, std::uint64_t size)
        : m_file(file), m_offset(offset), m_size(size)
    {
    }
    std::uint64_t size() const noexcept override { return m_size; }
    Result<ByteBuffer> read(std::uint64_t offset, std::size_t count) const override
    {
        if (offset > m_size || count > m_size - offset)
            return fail(ErrorCode::OutOfBounds, "read of {} bytes at {} is past the end ({} bytes)", count, offset,
                        m_size);
        return m_file.read(m_offset + offset, count);
    }

private:
    const RandomAccessFile& m_file;
    std::uint64_t m_offset;
    std::uint64_t m_size;
};

/// Owns the file it reads from.
class FileSource final : public SliceSource {
public:
    explicit FileSource(std::unique_ptr<RandomAccessFile> file)
        : SliceSource(*file, 0, file->size()), m_owned(std::move(file))
    {
    }

private:
    std::unique_ptr<RandomAccessFile> m_owned;
};

} // namespace

std::shared_ptr<const ByteSource> memorySource(ByteBuffer bytes)
{
    return std::make_shared<MemorySource>(std::move(bytes));
}

std::shared_ptr<const ByteSource> fileSliceSource(const RandomAccessFile& file, std::uint64_t offset,
                                                  std::uint64_t size)
{
    return std::make_shared<SliceSource>(file, offset, size);
}

Result<std::shared_ptr<const ByteSource>> fileSource(const std::filesystem::path& path)
{
    auto file = RandomAccessFile::open(path);
    if (!file)
        return std::unexpected(file.error());
    return std::shared_ptr<const ByteSource>(std::make_shared<FileSource>(std::move(*file)));
}

} // namespace viewer
