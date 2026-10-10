#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace viewer {

/// Coarse grouping used for icons, filters and choosing a previewer.
enum class FileCategory {
    Unknown,
    Archive,
    Texture,
    Model,
    Animation,
    Audio,
    Video,
    Text,
    Data,
    Shader,
    Level,
    Font,
    Executable,
};

[[nodiscard]] std::string_view categoryName(FileCategory category) noexcept;

struct FileTypeInfo {
    std::string extension;   ///< Lowercase, no dot.
    std::string description; ///< "DirectDraw Surface texture".
    FileCategory category = FileCategory::Unknown;
};

/// Maps file extensions to descriptions/categories. Global defaults are
/// registered first; game modules may override them with game-specific meanings.
class FileTypeRegistry {
public:
    void add(FileTypeInfo info);
    [[nodiscard]] const FileTypeInfo* find(std::string_view extension) const;
    /// Never null: falls back to a generic "<EXT> file" entry.
    [[nodiscard]] FileTypeInfo lookup(const std::filesystem::path& path) const;

private:
    std::unordered_map<std::string, FileTypeInfo> m_types;
};

/// Lowercase extension without the dot ("Foo.DDS" -> "dds").
[[nodiscard]] std::string extensionOf(const std::filesystem::path& path);
[[nodiscard]] std::string extensionOf(std::string_view path);
[[nodiscard]] inline std::string extensionOf(const std::string& path)
{
    return extensionOf(std::string_view(path));
}
[[nodiscard]] std::string toLower(std::string_view s);

} // namespace viewer
