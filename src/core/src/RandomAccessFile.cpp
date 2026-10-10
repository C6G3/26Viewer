#include "viewer/core/RandomAccessFile.hpp"

#include <algorithm>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace viewer {

namespace {

std::string displayPath(const std::filesystem::path& p)
{
    auto u8 = p.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

} // namespace

Result<std::unique_ptr<RandomAccessFile>> RandomAccessFile::open(const std::filesystem::path& path)
{
    std::unique_ptr<RandomAccessFile> file(new RandomAccessFile());
    file->m_path = path;
#ifdef _WIN32
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail(ErrorCode::Io, "cannot open '{}' (error {})", displayPath(path), ::GetLastError());
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        return fail(ErrorCode::Io, "cannot query size of '{}'", displayPath(path));
    }
    file->m_handle = h;
    file->m_size = static_cast<std::uint64_t>(size.QuadPart);
#else
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fail(ErrorCode::Io, "cannot open '{}'", displayPath(path));
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return fail(ErrorCode::Io, "cannot stat '{}'", displayPath(path));
    }
    file->m_fd = fd;
    file->m_size = static_cast<std::uint64_t>(st.st_size);
#endif
    return file;
}

RandomAccessFile::~RandomAccessFile()
{
#ifdef _WIN32
    if (m_handle)
        ::CloseHandle(static_cast<HANDLE>(m_handle));
#else
    if (m_fd >= 0)
        ::close(m_fd);
#endif
}

Result<void> RandomAccessFile::readAt(std::uint64_t offset, std::span<std::byte> out) const
{
    if (offset > m_size || out.size() > m_size - offset)
        return fail(ErrorCode::OutOfBounds, "read of {} bytes at {} exceeds file size {} ('{}')", out.size(),
                    offset, m_size, displayPath(m_path));

    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint64_t pos = offset + done;
        // Single reads are capped at 1 GiB to fit the Win32 DWORD length.
        const std::size_t chunk = std::min<std::size_t>(out.size() - done, std::size_t{1} << 30);
#ifdef _WIN32
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(m_handle), out.data() + done, static_cast<DWORD>(chunk), &got, &ov)
            || got == 0)
            return fail(ErrorCode::Io, "read failed at {} in '{}'", pos, displayPath(m_path));
#else
        const ssize_t got = ::pread(m_fd, out.data() + done, chunk, static_cast<off_t>(pos));
        if (got <= 0)
            return fail(ErrorCode::Io, "read failed at {} in '{}'", pos, displayPath(m_path));
#endif
        done += static_cast<std::size_t>(got);
    }
    return {};
}

Result<ByteBuffer> RandomAccessFile::read(std::uint64_t offset, std::size_t count) const
{
    ByteBuffer buffer(count);
    if (auto r = readAt(offset, buffer); !r)
        return std::unexpected(r.error());
    return buffer;
}

Result<ByteBuffer> readFile(const std::filesystem::path& path, std::uint64_t limit)
{
    auto file = RandomAccessFile::open(path);
    if (!file)
        return std::unexpected(file.error());
    const auto count = static_cast<std::size_t>(std::min((*file)->size(), limit));
    return (*file)->read(0, count);
}

Result<void> writeFile(const std::filesystem::path& path, ByteSpan data)
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return fail(ErrorCode::Io, "cannot create folder '{}': {}", displayPath(path.parent_path()),
                    ec.message());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return fail(ErrorCode::Io, "cannot create '{}'", displayPath(path));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out)
        return fail(ErrorCode::Io, "write failed for '{}'", displayPath(path));
    return {};
}

} // namespace viewer
