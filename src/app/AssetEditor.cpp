#include "AssetEditor.hpp"

#include "QtConvert.hpp"

#include "viewer/core/RandomAccessFile.hpp"

#include <QFileInfo>
#include <QLocale>
#include <QObject>
#include <QtConcurrent/QtConcurrentRun>

namespace viewer::app {

namespace {

QString tr(const char* text)
{
    return QObject::tr(text);
}

} // namespace

Result<ReplacePlan> planReplace(const Session& session, const VfsNode& node, const QString& file)
{
    if (!Vfs::canReplace(node))
        return fail(ErrorCode::Unsupported, "{}", tr("This item can't be replaced.").toStdString());

    const QString targetExt = toQString(extensionOf(node.name()));
    const QString sourceExt = QFileInfo(file).suffix().toLower();
    if (targetExt != sourceExt) {
        return fail(ErrorCode::InvalidFormat, "{}",
                    tr("File types must match: %1 is a .%2 file, but %3 is .%4.")
                        .arg(toQString(node.name()), targetExt, QFileInfo(file).fileName(),
                             sourceExt.isEmpty() ? tr("(no extension)") : sourceExt)
                        .toStdString());
    }

    auto data = readFile(toPath(file));
    if (!data)
        return std::unexpected(data.error());

    ReplacePlan plan;
    plan.node = &node;
    plan.sourceFile = file;
    plan.data = std::move(*data);

    // If we know how to decode this type, make sure the new file decodes too —
    // catches e.g. a renamed PNG pretending to be a .texture.
    const std::string ext = extensionOf(node.name());
    if (const ImageDecoder* decoder = session.content.findImageDecoder(ext, plan.data)) {
        if (auto img = decoder->decode(plan.data); !img)
            plan.warning = tr("The new file doesn't decode as a valid %1 (%2).")
                               .arg(toQString(decoder->name()), toQString(img.error().message));
    } else if (const StructureParser* parser = session.content.findStructureParser(ext, plan.data); !parser) {
        // The original parsed as a structured file but the replacement doesn't.
        if (auto original = session.vfs.read(node, 4096))
            if (session.content.findStructureParser(ext, *original))
                plan.warning = tr("The new file doesn't have the internal structure the original has.");
    }
    return plan;
}

std::filesystem::path backupTarget(const VfsNode& node)
{
    if (node.kind() == VfsNode::Kind::Archive)
        return node.diskPath();
    if (node.isFile())
        return Vfs::editTarget(node);
    // A folder inside an archive is protected by that archive's backup.
    for (const VfsNode* n = node.parent(); n; n = n->parent())
        if (n->kind() == VfsNode::Kind::Archive)
            return n->diskPath();
    return {};
}

bool hasBackup(const VfsNode& node)
{
    const auto target = backupTarget(node);
    std::error_code ec;
    return !target.empty() && std::filesystem::exists(backupPathFor(target), ec);
}

QFuture<EditResult> startBackup(std::filesystem::path target)
{
    return QtConcurrent::run([target = std::move(target)] {
        EditResult result;
        result.kind = EditResult::Kind::Backup;
        if (auto backup = ensureBackup(target); !backup)
            result.error = toQString(backup.error().message);
        else
            result.summary = QObject::tr("Backed up %1 to %2")
                                 .arg(toQString(target.filename()), toQString(backup->filename()));
        return result;
    });
}

QFuture<EditResult> startReplace(std::shared_ptr<Session> session, ReplacePlan plan)
{
    return QtConcurrent::run([session = std::move(session), plan = std::move(plan)] {
        EditResult result;
        result.kind = EditResult::Kind::Replace;
        if (plan.backupFirst) {
            if (auto backup = ensureBackup(Vfs::editTarget(*plan.node)); !backup) {
                result.error = toQString(backup.error().message);
                return result;
            }
        }
        auto report = session->vfs.replace(*plan.node, plan.data);
        if (!report) {
            result.error = toQString(report.error().message);
            return result;
        }
        const QLocale locale;
        result.summary = QObject::tr("Replaced %1 with %2 (%3, stored as %4)")
                             .arg(toQString(plan.node->name()), QFileInfo(plan.sourceFile).fileName(),
                                  locale.formattedDataSize(static_cast<qint64>(plan.data.size())),
                                  toQString(report->storage));
        for (const auto& w : report->warnings)
            result.warnings << toQString(w);
        return result;
    });
}

QFuture<EditResult> startRestore(std::filesystem::path target)
{
    return QtConcurrent::run([target = std::move(target)] {
        EditResult result;
        result.kind = EditResult::Kind::Restore;
        if (auto ok = restoreBackup(target); !ok)
            result.error = toQString(ok.error().message);
        else
            result.summary = QObject::tr("Restored the original %1").arg(toQString(target.filename()));
        return result;
    });
}

} // namespace viewer::app
