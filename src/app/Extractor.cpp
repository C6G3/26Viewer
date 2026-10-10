#include "Extractor.hpp"

#include "QtConvert.hpp"

#include "viewer/core/RandomAccessFile.hpp"

#include <QPromise>
#include <QtConcurrent/QtConcurrentRun>

#include <unordered_set>

namespace viewer::app {

std::vector<ExtractItem> planExtraction(const std::vector<const VfsNode*>& selection)
{
    std::vector<ExtractItem> items;
    std::unordered_set<const VfsNode*> seen; // a file may be selected and inside a selected folder
    for (const VfsNode* root : selection) {
        if (!root)
            continue;
        const VfsNode* base = root->parent();
        for (const VfsNode* file : Vfs::collectFiles(*root))
            if (seen.insert(file).second)
                items.push_back({file, file->pathRelativeTo(base)});
    }
    return items;
}

QFuture<ExtractSummary> startExtraction(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                        std::filesystem::path outputDir)
{
    return QtConcurrent::run(
        [session = std::move(session), items = std::move(items),
         outputDir = std::move(outputDir)](QPromise<ExtractSummary>& promise) {
            ExtractSummary summary;
            promise.setProgressRange(0, static_cast<int>(items.size()));
            int done = 0;
            for (const auto& item : items) {
                if (promise.isCanceled()) {
                    summary.canceled = true;
                    break;
                }
                promise.setProgressValueAndText(done, toQString(item.node->name()));

                const auto dest = outputDir / sanitizeRelativePath(item.relativePath);
                auto data = session->vfs.read(*item.node);
                Result<void> written = data ? writeFile(dest, *data) : Result<void>(std::unexpected(data.error()));
                if (written) {
                    ++summary.written;
                    summary.bytes += data->size();
                } else {
                    ++summary.failed;
                    if (summary.errors.size() < 20)
                        summary.errors << QStringLiteral("%1: %2").arg(toQString(item.relativePath),
                                                                       toQString(written.error().message));
                }
                ++done;
            }
            promise.setProgressValue(done);
            promise.addResult(summary);
        });
}

} // namespace viewer::app
