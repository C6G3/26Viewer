#pragma once

#include "Session.hpp"

#include <QFuture>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace viewer::app {

struct ExtractItem {
    const VfsNode* node = nullptr;
    std::string relativePath; ///< Where it lands under the output folder.
};

struct ExtractSummary {
    int written = 0;
    int failed = 0;
    int converted = 0; ///< Export: files written in another format (.wav, .obj, .png...).
    std::uint64_t bytes = 0;
    bool canceled = false;
    QStringList errors; ///< First few failures, for the report dialog.
};

/// Expands a selection (files, folders, archives) into individual files,
/// laid out relative to each selected item's parent.
std::vector<ExtractItem> planExtraction(const std::vector<const VfsNode*>& selection);

/// What one item of a job produced.
struct ItemDone {
    int files = 1;          ///< Files written for it (an export can write several).
    bool converted = false; ///< Written in another format.
};

struct ParallelOptions {
    int threads = 4;
    /// Items this big run on their own (nothing else at the same time), so a
    /// job of huge files (levels) doesn't hold several in memory at once.
    std::uint64_t exclusiveSize = ~std::uint64_t{0};
};

/// Runs `work` for every item on several threads and sums up the results.
/// The future reports progress (0..items) and supports cancel(); the Session
/// is kept alive until the job ends.
QFuture<ExtractSummary> runItems(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                 std::function<Result<ItemDone>(const Session&, const ExtractItem&)> work,
                                 ParallelOptions options = {});

/// Extracts on worker threads (several files at once). The future reports progress (0..items) and
/// supports cancel(); the Session is kept alive until the job ends.
QFuture<ExtractSummary> startExtraction(std::shared_ptr<Session> session, std::vector<ExtractItem> items,
                                        std::filesystem::path outputDir);

} // namespace viewer::app
