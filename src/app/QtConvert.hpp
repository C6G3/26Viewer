#pragma once

// Small conversions between the Qt-free core (UTF-8 std::string,
// std::filesystem::path) and Qt types.

#include <QString>

#include <filesystem>
#include <string>
#include <string_view>

namespace viewer::app {

inline QString toQString(std::string_view utf8)
{
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

// Exact overloads: std::string and const char* convert to both string_view and path.
inline QString toQString(const std::string& utf8)
{
    return toQString(std::string_view(utf8));
}

inline QString toQString(const char* utf8)
{
    return toQString(std::string_view(utf8));
}

inline QString toQString(const std::filesystem::path& path)
{
    const auto u8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()), static_cast<qsizetype>(u8.size()));
}

inline std::filesystem::path toPath(const QString& s)
{
    const QByteArray u8 = s.toUtf8();
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(u8.constData()), static_cast<std::size_t>(u8.size())));
}

} // namespace viewer::app
