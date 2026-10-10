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
};

/// A replacement the user has picked and that passed validation.
struct ReplacePlan {
    const VfsNode* node = nullptr;
    QString sourceFile;
    ByteBuffer data;
    QString warning; ///< Non-fatal concern to confirm (e.g. "doesn't decode as a texture").
    bool backupFirst = false; ///< Save the original as .bak before writing.
};

/// Checks that `file` can replace `node`: same extension, readable, and — when
/// the type has a decoder — that the new file actually decodes. Returns the
/// plan, or an error message suitable for showing to the user.
Result<ReplacePlan> planReplace(const Session& session, const VfsNode& node, const QString& file);

/// The same checks for new contents already in memory (e.g. text edited in
/// the editor); `sourceName` describes where they came from.
Result<ReplacePlan> planReplaceData(const Session& session, const VfsNode& node, ByteBuffer data,
                                    const QString& sourceName);

/// The file on disk whose backup protects `node`: its archive, the archive
/// itself for archive nodes, or the loose file. Empty for folders on disk.
[[nodiscard]] std::filesystem::path backupTarget(const VfsNode& node);

/// True if `node`'s backup target has a .bak original.
[[nodiscard]] bool hasBackup(const VfsNode& node);

/// Saves `target` as `<target>.bak` on a worker thread (never overwrites an existing backup).
QFuture<EditResult> startBackup(std::filesystem::path target);

/// Writes the replacement on a worker thread (optional backup → encode → write).
QFuture<EditResult> startReplace(std::shared_ptr<Session> session, ReplacePlan plan);

/// Copies the backup over the archive / loose file on a worker thread. The
/// session using that file must already be closed.
QFuture<EditResult> startRestore(std::filesystem::path target);

} // namespace viewer::app
