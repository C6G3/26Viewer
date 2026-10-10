#pragma once

#include "viewer/core/Bytes.hpp"

#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace test {

/// Little-endian byte builder for synthetic test files.
struct Writer {
    viewer::ByteBuffer bytes;

    template <typename T>
        requires std::is_integral_v<T>
    void put(T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF));
    }
    void put(std::string_view s, bool nul = false)
    {
        for (char c : s)
            bytes.push_back(static_cast<std::byte>(c));
        if (nul)
            bytes.push_back(std::byte{0});
    }
    void put(viewer::ByteSpan data) { bytes.insert(bytes.end(), data.begin(), data.end()); }
    void padTo(std::size_t alignment)
    {
        while (bytes.size() % alignment)
            bytes.push_back(std::byte{0});
    }
    template <typename T>
    void patch(std::size_t offset, T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes[offset + i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFF);
    }
};

inline viewer::ByteBuffer deflate(viewer::ByteSpan input, bool zlibHeader)
{
    z_stream zs{};
    deflateInit2(&zs, 9, Z_DEFLATED, zlibHeader ? 15 : -15, 9, Z_DEFAULT_STRATEGY);
    viewer::ByteBuffer out(deflateBound(&zs, static_cast<uLong>(input.size())));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

/// Pseudo-random but compressible test data.
inline viewer::ByteBuffer sampleData(std::size_t size, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    viewer::ByteBuffer out(size);
    for (std::size_t i = 0; i < size; ++i)
        out[i] = static_cast<std::byte>((i / 7) % 26 + 'a' + (rng() % 4 == 0 ? 1 : 0));
    return out;
}

/// A file in the system temp dir, removed when the guard dies.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name, viewer::ByteSpan data)
        : path(std::filesystem::temp_directory_path() / ("26viewer_test_" + name))
    {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace test
