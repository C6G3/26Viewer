#include "Extractor.hpp"

#include "QtConvert.hpp"

#include "viewer/core/RandomAccessFile.hpp"

#include <QPromise>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
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

QFuture<ExtractSummary> runItems(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                 std::function<Result<ItemDone>(const Session&, const ExtractItem&)> work,
                                 ParallelOptions options)
{
    return QtConcurrent::run([session = std::move(session), items = std::move(items), work = std::move(work),
                              options](QPromise<ExtractSummary>& promise) {
        ExtractSummary summary;
        promise.setProgressRange(0, static_cast<int>(items.size()));
        std::mutex mutex;              // guards summary, done and the progress report
        std::shared_mutex exclusive;   // big items take it alone, the rest share it
        std::atomic<std::size_t> next{0};
        int done = 0;

        const auto worker = [&] {
            for (std::size_t i; (i = next.fetch_add(1)) < items.size();) {
                if (promise.isCanceled())
                    return;
                const ExtractItem& item = items[i];
                Result<ItemDone> result;
                if (item.node->size() >= options.exclusiveSize) {
                    std::unique_lock lock(exclusive);
                    result = work(*session, item);
                } else {
                    std::shared_lock lock(exclusive);
                    result = work(*session, item);
                }
                const std::lock_guard lock(mutex);
                if (result) {
                    summary.written += result->files;
                    summary.converted += result->converted ? 1 : 0;
                    summary.bytes += item.node->size();
                } else {
                    ++summary.failed;
                    if (summary.errors.size() < 20)
                        summary.errors << QStringLiteral("%1: %2").arg(toQString(item.relativePath),
                                                                       toQString(result.error().message));
                }
                promise.setProgressValueAndText(++done, toQString(item.node->name()));
            }
        };
        const int threads = std::clamp(options.threads, 1, std::max(1, static_cast<int>(items.size())));
        {
            std::vector<std::jthread> pool;
            for (int t = 1; t < threads; ++t)
                pool.emplace_back(worker);
            worker(); // this thread works too
        } // joins the pool
        summary.canceled = promise.isCanceled() && done < static_cast<int>(items.size());
        promise.addResult(summary);
    });
}

QFuture<ExtractSummary> startExtraction(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                        std::filesystem::path outputDir)
{
    // Plain copies: a few at once keeps the disk busy without thrashing it.
    return runItems(std::move(session), std::move(items),
                    [outputDir = std::move(outputDir)](const Session& job,
                                                       const ExtractItem& item) -> Result<ItemDone> {
                        auto data = job.vfs.read(*item.node);
                        if (!data)
                            return std::unexpected(data.error());
                        if (auto ok = writeFile(outputDir / sanitizeRelativePath(item.relativePath), *data); !ok)
                            return std::unexpected(ok.error());
                        return ItemDone{};
                    },
                    {.threads = 4});
}

} // namespace viewer::app
