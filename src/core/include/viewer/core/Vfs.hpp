#pragma once

#include "viewer/core/Archive.hpp"
#include "viewer/core/ContentRegistry.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

/// One node of the virtual file system shown in the browser tree.
///
/// The tree mirrors the game's install folder; every recognised archive
/// becomes an expandable node holding its own contents, so loose files and
/// packed files are browsed the same way.
class VfsNode {
public:
    enum class Kind { Folder, Archive, File };

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool isFile() const noexcept { return m_kind == Kind::File; }
    [[nodiscard]] bool isContainer() const noexcept { return m_kind != Kind::File; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] VfsNode* parent() const noexcept { return m_parent; }
    [[nodiscard]] const std::vector<std::unique_ptr<VfsNode>>& children() const noexcept { return m_children; }
    [[nodiscard]] std::size_t row() const noexcept { return m_row; }

    /// File size (uncompressed); for containers, the total of all files below.
    [[nodiscard]] std::uint64_t size() const noexcept { return m_size; }
    [[nodiscard]] std::uint64_t packedSize() const noexcept { return m_packedSize; }
    [[nodiscard]] std::size_t fileCount() const noexcept { return m_fileCount; }
    [[nodiscard]] const auto& modified() const noexcept { return m_modified; }

    /// The archive this node lives in (or, for Archive nodes, the archive itself).
    [[nodiscard]] const Archive* archive() const noexcept { return m_archive; }
    [[nodiscard]] std::size_t entryIndex() const noexcept { return m_entryIndex; }
    /// On-disk location for loose files and archive nodes; empty otherwise.
    [[nodiscard]] const std::filesystem::path& diskPath() const noexcept { return m_diskPath; }

    /// "cache/root.pak/models/a.model"
    [[nodiscard]] std::string virtualPath() const;
    /// Path relative to an ancestor (used to lay out extracted files).
    [[nodiscard]] std::string pathRelativeTo(const VfsNode* ancestor) const;

private:
    friend class Vfs;

    Kind m_kind = Kind::Folder;
    std::string m_name;
    VfsNode* m_parent = nullptr;
    std::vector<std::unique_ptr<VfsNode>> m_children;
    std::unordered_map<std::string, VfsNode*> m_lookup; // lowercase name -> child
    std::size_t m_row = 0;

    std::uint64_t m_size = 0;
    std::uint64_t m_packedSize = 0;
    std::size_t m_fileCount = 0;
    std::optional<std::chrono::system_clock::time_point> m_modified;

    const Archive* m_archive = nullptr;
    std::size_t m_entryIndex = 0;
    std::filesystem::path m_diskPath;
};

struct MountOptions {
    /// Directory names (lowercase) skipped while scanning.
    std::vector<std::string> ignoredDirectories;
    /// Called for each archive as it is opened; return false to cancel.
    std::function<bool(const std::filesystem::path&)> onArchive;
};

struct MountReport {
    std::size_t archives = 0;
    std::size_t files = 0;
    std::vector<Error> warnings; ///< Archives that failed to open, etc.
};

/// Owns opened archives and the browsable node tree built from them.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    [[nodiscard]] const VfsNode& root() const noexcept { return *m_root; }

    /// Scans `directory`: loose files become File nodes, recognised archives
    /// become Archive nodes with their contents mounted beneath.
    MountReport mountDirectory(const std::filesystem::path& directory, const ContentRegistry& content,
                               const MountOptions& options = {});

    /// Mounts a single archive file at the top level.
    Result<const VfsNode*> mountArchive(const std::filesystem::path& file, const ContentRegistry& content);

    /// Reads a file node. Reads at most `limit` bytes of loose files (archive
    /// entries are always read whole because they are compressed).
    [[nodiscard]] Result<ByteBuffer> read(const VfsNode& node, std::uint64_t limit = UINT64_MAX) const;

    /// Every file node at or beneath `node`, in tree order.
    [[nodiscard]] static std::vector<const VfsNode*> collectFiles(const VfsNode& node);

private:
    VfsNode* addChild(VfsNode& parent, std::string_view name, VfsNode::Kind kind);
    VfsNode* ensureFolder(VfsNode& parent, std::string_view relativePath);
    void populateArchive(VfsNode& archiveNode, const Archive& archive);
    static void finalize(VfsNode& node);

    std::unique_ptr<VfsNode> m_root;
    std::vector<std::unique_ptr<Archive>> m_archives;
};

/// Removes "..", drive letters and leading slashes so an archive entry can
/// never be written outside the chosen output folder.
[[nodiscard]] std::filesystem::path sanitizeRelativePath(std::string_view path);

} // namespace viewer
