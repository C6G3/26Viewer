#pragma once

#include "Session.hpp"

#include <QFuture>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <memory>

namespace viewer::app {

/// Outcome of a background edit operation.
struct EditResult {
    enum class Kind { Replace, Restore, Backup };
    Kind kind = Kind::Replace;
    QString error;   ///< Empty on success.
    QString summary; ///< One line for the status bar.
    QStringList warnings;
    bool unchanged = false; ///< Nothing needed writing (e.g. the file already was the original).
};

/// A replacement the user has picked and that passed validation.
struct ReplacePlan {
    const VfsNode* node = nullptr;
    QString sourceFile;
    ByteBuffer data;
    QString warning; ///< Non-fatal concern to confirm (e.g. "doesn't decode as a texture").
    QStringList notes; ///< Extra facts shown when confirming (e.g. "3 sounds replaced, 40 kept").
    bool backupFirst = false; ///< Save the original in the backup folder before writing.
};

/// Checks that `file` can replace `node`: same extension, readable, and — when
/// the type has a decoder — that the new file actually decodes. Returns the
/// plan, or an error message suitable for showing to the user.
Result<ReplacePlan> planReplace(const Session& session, const VfsNode& node, const QString& file);

/// The same checks for new contents already in memory (e.g. text edited in
/// the editor); `sourceName` describes where they came from.
Result<ReplacePlan> planReplaceData(const Session& session, const VfsNode& node, ByteBuffer data,
                                    const QString& sourceName);

/// Replace from Folder: the files of a folder laid out like part of the tree,
/// each matched to the file at the same path (letter case ignored).
struct BatchReplace {
    struct Item {
        const VfsNode* node = nullptr;
        QString sourceFile;
    };
    std::vector<Item> items;
    QStringList unmatched;  ///< Files in the folder with no counterpart (relative paths).
    std::vector<std::filesystem::path> targets; ///< Archives / loose files that will change, once each.
    bool backupFirst = false; ///< Back up targets that have no backup yet, before writing.
};

/// Matches the files under `folder` to the replaceable files under `target`.
/// A top folder named like `target` (an extracted copy of it) is looked into.
/// Only paths are compared here; contents are checked when the job runs.
[[nodiscard]] BatchReplace planBatchReplace(const VfsNode& target, const std::filesystem::path& folder);

/// Writes a batch on a worker thread: optional backups first, then each file
/// that passes the planReplace checks. Files identical to what is there now
/// are left alone (each replacement adds its data to the archive); files that
/// don't decode like the original are skipped and listed.
QFuture<EditResult> startBatchReplace(std::shared_ptr<Session> session, BatchReplace batch);

/// The file on disk whose backup protects `node`: its archive, the archive
/// itself for archive nodes, or the loose file. Empty for folders on disk.
[[nodiscard]] std::filesystem::path backupTarget(const VfsNode& node);

/// The backup of `node`'s backup target (in "<game>/backup", or an older
/// ".bak" beside it), or empty when there is none.
[[nodiscard]] std::filesystem::path backupOf(const Session& session, const VfsNode& node);
[[nodiscard]] bool hasBackup(const Session& session, const VfsNode& node);

/// `backup` as shown to the user: relative to the game folder ("backup\cache\root.pak").
[[nodiscard]] QString backupDisplayName(const Session& session, const std::filesystem::path& backup);

/// True when Restore Original puts back just this one file (taken from the
/// backup of its archive) rather than a whole archive or loose file.
[[nodiscard]] bool restoresSingleFile(const VfsNode& node) noexcept;

/// Copies `target` into the backup folder under `root` on a worker thread
/// (never overwrites an existing backup).
QFuture<EditResult> startBackup(std::filesystem::path target, std::filesystem::path root);

/// Writes the replacement on a worker thread (optional backup → encode → write).
QFuture<EditResult> startReplace(std::shared_ptr<Session> session, ReplacePlan plan);

/// Copies the backup over the archive / loose file on a worker thread. The
/// session using that file must already be closed.
QFuture<EditResult> startRestore(std::filesystem::path target, std::filesystem::path root);

/// Puts back one file inside an archive as it is in the archive's backup,
/// on a worker thread (the rest of the archive keeps its changes).
QFuture<EditResult> startRestoreFile(std::shared_ptr<Session> session, const VfsNode* node);

} // namespace viewer::app
