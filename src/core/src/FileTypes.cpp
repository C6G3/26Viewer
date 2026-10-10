#include "viewer/core/FileTypes.hpp"

#include <algorithm>
#include <cctype>

namespace viewer {

std::string_view categoryName(FileCategory category) noexcept
{
    switch (category) {
    case FileCategory::Unknown: return "Unknown";
    case FileCategory::Archive: return "Archive";
    case FileCategory::Texture: return "Texture";
    case FileCategory::Model: return "Model";
    case FileCategory::Animation: return "Animation";
    case FileCategory::Audio: return "Audio";
    case FileCategory::Video: return "Video";
    case FileCategory::Text: return "Text";
    case FileCategory::Data: return "Data";
    case FileCategory::Shader: return "Shader";
    case FileCategory::Level: return "Level";
    case FileCategory::Font: return "Font";
    case FileCategory::Executable: return "Executable";
    }
    return "Unknown";
}

std::string toLower(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string extensionOf(std::string_view path)
{
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    return toLower(path.substr(dot + 1));
}

std::string extensionOf(const std::filesystem::path& path)
{
    const auto ext = path.extension().u8string();
    if (ext.size() <= 1)
        return {};
    return toLower(std::string_view(reinterpret_cast<const char*>(ext.data()) + 1, ext.size() - 1));
}

void FileTypeRegistry::add(FileTypeInfo info)
{
    info.extension = toLower(info.extension);
    auto key = info.extension;
    m_types.insert_or_assign(std::move(key), std::move(info));
}

const FileTypeInfo* FileTypeRegistry::find(std::string_view extension) const
{
    const auto it = m_types.find(toLower(extension));
    return it == m_types.end() ? nullptr : &it->second;
}

FileTypeInfo FileTypeRegistry::lookup(const std::filesystem::path& path) const
{
    const auto ext = extensionOf(path);
    if (const auto* info = find(ext))
        return *info;
    if (ext.empty())
        return {"", "File", FileCategory::Unknown};
    return {ext, toLower(ext).insert(0, ".") + " file", FileCategory::Unknown};
}

} // namespace viewer
