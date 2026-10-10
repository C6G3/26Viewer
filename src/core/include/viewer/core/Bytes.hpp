#pragma once

#include "viewer/core/Error.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace viewer {

using ByteBuffer = std::vector<std::byte>;
using ByteSpan = std::span<const std::byte>;

inline ByteSpan asBytes(std::string_view s) noexcept
{
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/// Reads a little- or big-endian scalar at `offset`. Caller guarantees bounds.
template <typename T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] T loadScalar(ByteSpan data, std::size_t offset, std::endian order = std::endian::little) noexcept
{
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    if constexpr (sizeof(T) > 1) {
        if (order != std::endian::native) {
            if constexpr (std::is_integral_v<T>) {
                value = std::byteswap(value);
            } else {
                using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
                value = std::bit_cast<T>(std::byteswap(std::bit_cast<U>(value)));
            }
        }
    }
    return value;
}

/// Bounds-checked sequential reader over an in-memory buffer.
///
///     BinaryReader r(bytes);
///     auto magic = r.read<std::uint32_t>();   // Result<uint32_t>
///     if (!magic) return std::unexpected(magic.error());
class BinaryReader {
public:
    explicit BinaryReader(ByteSpan data, std::endian order = std::endian::little) noexcept
        : m_data(data), m_order(order)
    {
    }

    [[nodiscard]] std::size_t position() const noexcept { return m_pos; }
    [[nodiscard]] std::size_t size() const noexcept { return m_data.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    [[nodiscard]] ByteSpan data() const noexcept { return m_data; }
    void setEndian(std::endian order) noexcept { m_order = order; }

    Result<void> seek(std::size_t pos)
    {
        if (pos > m_data.size())
            return fail(ErrorCode::OutOfBounds, "seek to {} past end ({})", pos, m_data.size());
        m_pos = pos;
        return {};
    }

    Result<void> skip(std::size_t count) { return seek(m_pos + count); }

    template <typename T>
        requires std::is_arithmetic_v<T>
    Result<T> read()
    {
        if (remaining() < sizeof(T))
            return fail(ErrorCode::OutOfBounds, "read of {} bytes at {} past end ({})", sizeof(T), m_pos,
                        m_data.size());
        T value = loadScalar<T>(m_data, m_pos, m_order);
        m_pos += sizeof(T);
        return value;
    }

    Result<ByteSpan> readBytes(std::size_t count)
    {
        if (remaining() < count)
            return fail(ErrorCode::OutOfBounds, "read of {} bytes at {} past end ({})", count, m_pos,
                        m_data.size());
        ByteSpan out = m_data.subspan(m_pos, count);
        m_pos += count;
        return out;
    }

    /// Reads exactly `count` bytes as a string, trimming at the first NUL.
    Result<std::string> readFixedString(std::size_t count)
    {
        auto bytes = readBytes(count);
        if (!bytes)
            return std::unexpected(bytes.error());
        std::string s(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        if (auto nul = s.find('\0'); nul != std::string::npos)
            s.resize(nul);
        return s;
    }

private:
    ByteSpan m_data;
    std::size_t m_pos = 0;
    std::endian m_order;
};

/// Reads a NUL-terminated string starting at `offset` (bounded by the span).
[[nodiscard]] std::string_view cstringAt(ByteSpan data, std::size_t offset) noexcept;

/// Formats a byte count for humans: "1.4 MiB".
[[nodiscard]] std::string formatSize(std::uint64_t bytes);

} // namespace viewer
