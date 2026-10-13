#pragma once

#include "../Session.hpp"

#include "viewer/core/Animation.hpp"
#include "viewer/core/Mesh.hpp"

#include <QDialog>
#include <QFutureWatcher>
#include <QModelIndex>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTreeView;

namespace viewer::app {

/// The skeleton a model is posed with, and where it came from.
struct SkeletonMatch {
    std::shared_ptr<const Skeleton> skeleton;
    QString source; ///< "animations/igm_driver_ml.skeleton", or a description.
    /// An animation that belongs to the model (same name), loaded but not playing.
    std::shared_ptr<const Animation> animation;
    QString animationSource;
};

/// Finds the skeleton for `mesh` (opened from `model`): the game's skeleton
/// file holding every bone the model is skinned to (or, for a rigid model,
/// the most of its parts' names; one with the model's own name first). A
/// model without one is posed by its own parts when it has several. Runs on a
/// worker thread (reads files through the session).
[[nodiscard]] SkeletonMatch findSkeleton(const Session& session, const VfsNode& model, const Mesh& mesh);

/// Every file in the session an animation decoder may read (by file type).
[[nodiscard]] std::vector<const VfsNode*> animationFiles(const Session& session);
/// The animation file at `virtualPath` decoded, or null.
[[nodiscard]] std::shared_ptr<const Animation> loadAnimationAt(const Session& session, const QString& virtualPath);

class AnimationFilterModel;
class VfsModel;

/// Where the animation picker was left for one model (folders open, row
/// selected), so reopening it for that model goes back there. A new model
/// starts with an empty one.
struct AnimationPickerState {
    QStringList expanded; ///< Name paths of the open folders ("Blur/cache/root.pak/animations").
    QString current;      ///< Name path of the selected row.
    /// Before the picker was first used: the model's own animation (virtual path) to start at.
    QString startFile;
};

/// Picks an animation for a skeleton in a folder tree like the main window's,
/// showing only folders that hold animations. The ones made for other
/// skeletons are greyed out (and hidden unless "Show all" is ticked) once
/// they have been checked in the background.
class AnimationPicker : public QDialog {
    Q_OBJECT
public:
    /// `state` (may be null) is restored when the tree is filled and updated when the picker closes.
    AnimationPicker(std::shared_ptr<Session> session, std::shared_ptr<const Skeleton> skeleton,
                    AnimationPickerState* state, QWidget* parent = nullptr);
    ~AnimationPicker() override;

    /// The chosen file and its decoded animation (after exec() returned Accepted).
    [[nodiscard]] const VfsNode* selectedNode() const noexcept { return m_selected; }
    [[nodiscard]] std::shared_ptr<const Animation> selectedAnimation() const noexcept { return m_animation; }

    void done(int result) override;

private:
    /// Fills the tree once it's known which animations fit (no folders opening and closing).
    void showFits(std::vector<int> fits);
    void refilter();
    void accept() override;
    [[nodiscard]] const VfsNode* currentNode() const;
    /// Folders open in the tree, as name paths ("Blur/cache/root.pak/animations").
    [[nodiscard]] QStringList expandedFolders() const;
    /// The row at a name path, or the deepest folder of it still in the tree.
    [[nodiscard]] QModelIndex indexAtPath(const QString& path) const;
    [[nodiscard]] static QString pathOf(const QModelIndex& index);
    /// Opens `expanded` and selects `current` (where the picker was left last time).
    void restoreView(const QStringList& expanded, const QString& current);

    std::shared_ptr<Session> m_session;
    std::shared_ptr<const Skeleton> m_skeleton;
    AnimationPickerState* m_state;
    std::vector<const VfsNode*> m_files;
    std::vector<int> m_fits; ///< Per m_files: 1 fits the skeleton, 0 doesn't.
    bool m_ready = false;    ///< m_fits is known and the tree filled.
    bool m_searching = false;   ///< The tree shows search results (every folder open).
    QStringList m_beforeSearch; ///< Open folders when the search started (restored when it's cleared).
    QFutureWatcher<std::vector<int>> m_check;
    VfsModel* m_model;
    AnimationFilterModel* m_filter;
    QLineEdit* m_search;
    QCheckBox* m_showAll;
    QTreeView* m_tree;
    QLabel* m_status;
    const VfsNode* m_selected = nullptr;
    std::shared_ptr<const Animation> m_animation;
};

} // namespace viewer::app
