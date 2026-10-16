#include "viewer/core/Vfs.hpp"
#include "viewer/core/RandomAccessFile.hpp"

#include <algorithm>
#include <ranges>

namespace viewer {

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p)
{
    auto s = p.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::optional<std::chrono::system_clock::time_point> diskTime(const fs::directory_entry& e)
{
    std::error_code ec;
    const auto t = e.last_write_time(ec);
    if (ec)
        return std::nullopt;
    return std::chrono::clock_cast<std::chrono::system_clock>(t);
}

} // namespace

// ---- VfsNode -----------------------------------------------------------------

std::string VfsNode::virtualPath() const
{
    return pathRelativeTo(nullptr);
}

std::string VfsNode::pathRelativeTo(const VfsNode* ancestor) const
{
    std::vector<const VfsNode*> chain;
    for (const VfsNode* n = this; n && n != ancestor && n->m_parent; n = n->m_parent)
        chain.push_back(n);
    std::string out;
    for (const VfsNode* n : chain | std::views::reverse) {
        if (!out.empty())
            out += '/';
        out += n->m_name;
    }
    return out;
}

// ---- Vfs -----------------------------------------------------------------------

Vfs::Vfs() : m_root(std::make_unique<VfsNode>()) {}
Vfs::~Vfs() = default;

VfsNode* Vfs::addChild(VfsNode& parent, std::string_view name, VfsNode::Kind kind)
{
    auto node = std::make_unique<VfsNode>();
    node->m_kind = kind;
    node->m_name = std::string(name);
    node->m_parent = &parent;
    VfsNode* raw = node.get();
    parent.m_lookup[toLower(name)] = raw;
    parent.m_children.push_back(std::move(node));
    return raw;
}

VfsNode* Vfs::ensureFolder(VfsNode& parent, std::string_view relativePath)
{
    VfsNode* current = &parent;
    for (auto part : relativePath | std::views::split('/')) {
        const std::string_view name(part.begin(), part.end());
        if (name.empty())
            continue;
        const auto it = current->m_lookup.find(toLower(name));
        if (it != current->m_lookup.end() && it->second->isContainer())
            current = it->second;
        else
            current = addChild(*current, name, VfsNode::Kind::Folder);
    }
    return current;
}

void Vfs::populateArchive(VfsNode& archiveNode, const Archive& archive)
{
    archiveNode.m_archive = &archive;
    const auto entries = archive.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const auto slash = e.path.find_last_of('/');
        VfsNode* folder = slash == std::string::npos
                              ? &archiveNode
                              : ensureFolder(archiveNode, std::string_view(e.path).substr(0, slash));
        const auto name = slash == std::string::npos ? std::string_view(e.path)
                                                     : std::string_view(e.path).substr(slash + 1);
        VfsNode* file = addChild(*folder, name, VfsNode::Kind::File);
        file->m_archive = &archive;
        file->m_entryIndex = i;
        file->m_size = e.size;
        file->m_packedSize = e.packedSize;
        file->m_modified = e.modified;
    }
}

void Vfs::finalize(VfsNode& node)
{
    if (node.isFile()) {
        node.m_fileCount = 1;
        return;
    }
    std::uint64_t size = 0, packed = 0;
    std::size_t files = 0;
    for (auto& child : node.m_children) {
        finalize(*child);
        size += child->m_size;
        packed += child->m_packedSize;
        files += child->m_fileCount;
    }
    // Containers first, then case-insensitive name order.
    std::ranges::sort(node.m_children, [](const auto& a, const auto& b) {
        if (a->isContainer() != b->isContainer())
            return a->isContainer();
        return toLower(a->m_name) < toLower(b->m_name);
    });
    for (std::size_t i = 0; i < node.m_children.size(); ++i)
        node.m_children[i]->m_row = i;
    // Archive nodes keep their on-disk size as "packed" so the UI can show both.
    if (node.m_kind != VfsNode::Kind::Archive)
        node.m_packedSize = packed;
    node.m_size = size;
    node.m_fileCount = files;
}

MountReport Vfs::mountDirectory(const fs::path& directory, const ContentRegistry& content,
                                const MountOptions& options)
{
    MountReport report;
    if (m_backupRoot.empty())
        m_backupRoot = directory;
    std::error_code ec;
    fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        report.warnings.push_back({ErrorCode::Io, "cannot scan '" + u8(directory) + "': " + ec.message()});
        return report;
    }

    // Collect first so archives can be opened in a stable (sorted) order.
    std::vector<fs::directory_entry> files;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        const auto& entry = *it;
        if (entry.is_directory(ec)) {
            const auto name = toLower(u8(entry.path().filename()));
            // The originals kept in "<game>/backup" aren't part of the game.
            if (std::ranges::contains(options.ignoredDirectories, name) || (it.depth() == 0 && name == kBackupFolder))
                it.disable_recursion_pending();
            continue;
        }
        if (entry.is_regular_file(ec))
            files.push_back(entry);
    }
    std::ranges::sort(files, {}, [](const fs::directory_entry& e) { return e.path(); });

    for (const auto& entry : files) {
        const fs::path rel = entry.path().lexically_relative(directory);
        const auto parentU8 = rel.parent_path().generic_u8string();
        const std::string_view relParent(reinterpret_cast<const char*>(parentU8.data()), parentU8.size());
        VfsNode* folder = ensureFolder(*m_root, relParent);
        const std::string name = u8(rel.filename());
        const auto modified = diskTime(entry);
        const auto diskSize = entry.file_size(ec);

        if (const ArchiveFormat* format = content.findArchiveFormat(entry.path())) {
            if (options.onArchive && !options.onArchive(entry.path()))
                break;
            auto archive = format->open(entry.path());
            if (archive) {
                VfsNode* node = addChild(*folder, name, VfsNode::Kind::Archive);
                node->m_diskPath = entry.path();
                node->m_packedSize = diskSize;
                node->m_modified = modified;
                populateArchive(*node, **archive);
                m_archives.push_back(std::move(*archive));
                ++report.archives;
                continue;
            }
            report.warnings.push_back(archive.error());
        }

        VfsNode* file = addChild(*folder, name, VfsNode::Kind::File);
        file->m_diskPath = entry.path();
        file->m_size = file->m_packedSize = diskSize;
        file->m_modified = modified;
    }

    finalize(*m_root);
    report.files = m_root->fileCount();
    return report;
}

Result<const VfsNode*> Vfs::mountArchive(const fs::path& file, const ContentRegistry& content)
{
    const ArchiveFormat* format = content.findArchiveFormat(file);
    if (!format)
        return fail(ErrorCode::Unsupported, "'{}' is not an archive this build can open", u8(file.filename()));
    auto archive = format->open(file);
    if (!archive)
        return std::unexpected(archive.error());
    if (m_backupRoot.empty())
        m_backupRoot = file.parent_path();

    std::error_code ec;
    VfsNode* node = addChild(*m_root, u8(file.filename()), VfsNode::Kind::Archive);
    node->m_diskPath = file;
    node->m_packedSize = fs::file_size(file, ec);
    populateArchive(*node, **archive);
    m_archives.push_back(std::move(*archive));
    finalize(*m_root);
    return node;
}

Result<ByteBuffer> Vfs::readBackup(const VfsNode& node, const ContentRegistry& content) const
{
    if (!node.isFile())
        return fail(ErrorCode::InvalidFormat, "'{}' is not a file", node.name());
    const fs::path target = editTarget(node);
    const fs::path backup = findBackup(target, m_backupRoot);
    if (backup.empty())
        return fail(ErrorCode::NotFound, "'{}' has no backup", u8(target.filename()));
    if (!node.archive())
        return readFile(backup);
    // The backup is a copy of the archive: open it the way the archive itself opens.
    const ArchiveFormat* format = content.findArchiveFormat(target);
    if (!format)
        return fail(ErrorCode::Unsupported, "can't open the backup of '{}'", u8(target.filename()));
    auto original = format->open(backup);
    if (!original)
        return std::unexpected(original.error());
    const std::string wanted = toLower(node.archive()->entries()[node.entryIndex()].path);
    const auto entries = (*original)->entries();
    const auto it = std::ranges::find_if(entries, [&](const ArchiveEntry& e) { return toLower(e.path) == wanted; });
    if (it == entries.end())
        return fail(ErrorCode::NotFound, "'{}' isn't in the backup of '{}'", node.name(), u8(target.filename()));
    return (*original)->read(static_cast<std::size_t>(it - entries.begin()));
}

Result<ByteBuffer> Vfs::read(const VfsNode& node, std::uint64_t limit) const
{
    if (!node.isFile())
        return fail(ErrorCode::InvalidFormat, "'{}' is not a file", node.name());
    if (node.archive())
        return node.archive()->read(node.entryIndex());
    return readFile(node.diskPath(), limit);
}

HashToolContext Vfs::hashToolContext() const
{
    // The archives in the tree (the game folder itself counts for loose files).
    const auto archives = [this] {
        std::vector<const VfsNode*> out{m_root.get()};
        std::vector<const VfsNode*> stack{m_root.get()};
        while (!stack.empty()) {
            const VfsNode* n = stack.back();
            stack.pop_back();
            for (const auto& child : n->children()) {
                if (child->kind() == VfsNode::Kind::Archive)
                    out.push_back(child.get());
                else if (child->kind() == VfsNode::Kind::Folder)
                    stack.push_back(child.get());
            }
        }
        return out;
    };
    HashToolContext context;
    context.readFolder = [this, archives](std::string_view folder) {
        std::vector<DecompileContext::File> files;
        for (const VfsNode* archive : archives()) {
            auto found = decompileContext(*archive).readFolder(folder);
            std::ranges::move(found, std::back_inserter(files));
        }
        return files;
    };
    context.archivePaths = [archives] {
        std::vector<std::string> paths;
        for (const VfsNode* archive : archives()) {
            if (archive->kind() != VfsNode::Kind::Archive)
                continue;
            std::vector<const VfsNode*> stack{archive};
            while (!stack.empty()) {
                const VfsNode* n = stack.back();
                stack.pop_back();
                for (const auto& child : n->children()) {
                    paths.push_back(child->pathRelativeTo(archive));
                    if (child->isContainer())
                        stack.push_back(child.get());
                }
            }
        }
        return paths;
    };
    return context;
}

DecompileContext Vfs::decompileContext(const VfsNode& node) const
{
    const VfsNode* base = &node;
    while (base->parent() && base->kind() != VfsNode::Kind::Archive)
        base = base->parent();
    DecompileContext context;
    context.readFolder = [this, base](std::string_view folder) {
        std::vector<DecompileContext::File> files;
        const VfsNode* current = base;
        for (const auto part : std::views::split(folder, '/')) {
            const std::string_view name(part.begin(), part.end());
            if (name.empty())
                continue;
            const auto it = current->m_lookup.find(toLower(name));
            if (it == current->m_lookup.end())
                return files;
            current = it->second;
        }
        for (const auto& child : current->children())
            if (child->isFile())
                if (auto data = read(*child))
                    files.push_back({child->name(), std::move(*data)});
        return files;
    };
    context.readSibling = [this, file = &node](std::string_view name) -> std::optional<ByteBuffer> {
        const VfsNode* folder = file->parent();
        if (!folder)
            return std::nullopt;
        const auto it = folder->m_lookup.find(toLower(name));
        if (it == folder->m_lookup.end() || !it->second->isFile())
            return std::nullopt;
        auto data = read(*it->second);
        return data ? std::optional<ByteBuffer>(std::move(*data)) : std::nullopt;
    };
    return context;
}

Result<std::shared_ptr<const ByteSource>> Vfs::source(const VfsNode& node)
{
    if (!node.isFile())
        return fail(ErrorCode::InvalidFormat, "'{}' is not a file", node.name());
    if (node.archive())
        return node.archive()->openEntry(node.entryIndex());
    return fileSource(node.diskPath());
}

std::vector<const VfsNode*> Vfs::collectFiles(const VfsNode& node)
{
    std::vector<const VfsNode*> out;
    std::vector<const VfsNode*> stack{&node};
    while (!stack.empty()) {
        const VfsNode* n = stack.back();
        stack.pop_back();
        if (n->isFile()) {
            out.push_back(n);
            continue;
        }
        for (const auto& child : n->children() | std::views::reverse)
            stack.push_back(child.get());
    }
    return out;
}

bool Vfs::canReplace(const VfsNode& node) noexcept
{
    if (!node.isFile())
        return false;
    if (node.archive())
        return node.archive()->canReplace();
    return !node.diskPath().empty();
}

fs::path Vfs::editTarget(const VfsNode& node)
{
    if (node.archive())
        return node.archive()->path();
    return node.diskPath();
}

Result<ReplaceReport> Vfs::replace(const VfsNode& node, ByteSpan data) const
{
    if (!canReplace(node))
        return fail(ErrorCode::Unsupported, "'{}' cannot be replaced", node.name());

    const ScopedWritable writable(editTarget(node)); // game files are often read-only
    if (!writable.status())
        return std::unexpected(writable.status().error());

    Result<ReplaceReport> report = ReplaceReport{};
    if (node.archive()) {
        report = node.archive()->replace(node.entryIndex(), data);
    } else if (auto written = writeFile(node.diskPath(), data); written) {
        report->storage = "loose file";
        report->storedSize = data.size();
    } else {
        return std::unexpected(written.error());
    }
    if (report)
        report->backup = findBackup(editTarget(node), m_backupRoot);
    return report;
}

fs::path sanitizeRelativePath(std::string_view path)
{
    fs::path out;
    for (auto part : path | std::views::split('/')) {
        std::string name(part.begin(), part.end());
        std::ranges::replace(name, '\\', '_');
        std::ranges::replace(name, ':', '_');
        if (name.empty() || name == "." || name == "..")
            continue;
        out /= fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    }
    return out;
}

} // namespace viewer
