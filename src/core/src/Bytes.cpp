#include "viewer/core/Archive.hpp"
#include "viewer/core/Bytes.hpp"

#include <array>
#include <format>

namespace viewer {

std::string_view cstringAt(ByteSpan data, std::size_t offset) noexcept
{
    if (offset >= data.size())
        return {};
    const auto* begin = reinterpret_cast<const char*>(data.data() + offset);
    const std::size_t maxLen = data.size() - offset;
    std::size_t len = 0;
    while (len < maxLen && begin[len] != '\0')
        ++len;
    return {begin, len};
}

std::string formatSize(std::uint64_t bytes)
{
    static constexpr std::array units{"B", "KiB", "MiB", "GiB", "TiB"};
    if (bytes < 1024)
        return std::format("{} B", bytes);
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

std::string normalizeArchivePath(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '\\')
            c = '/';
        if (c == '/' && (out.empty() || out.back() == '/'))
            continue; // drop leading and repeated separators
        out.push_back(c);
    }
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

std::optional<std::chrono::system_clock::time_point> fromFileTime(std::uint64_t ticks) noexcept
{
    // FILETIME counts 100ns intervals since 1601-01-01; Unix time starts 1970-01-01.
    constexpr std::uint64_t kEpochDelta = 116'444'736'000'000'000ULL;
    if (ticks <= kEpochDelta || ticks == UINT64_MAX)
        return std::nullopt;
    const auto sinceUnix = std::chrono::microseconds((ticks - kEpochDelta) / 10);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceUnix));
}

} // namespace viewer
