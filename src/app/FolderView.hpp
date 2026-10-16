#pragma once

#include "viewer/core/Vfs.hpp"

#include <QWidget>

#include <vector>

class QLabel;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace viewer::app {

class BackupBanner;
class VfsFilterModel;
class VfsModel;

/// The right-hand folder list: shows what is
/// inside the folder or archive selected in the navigation tree. Double-click
/// (or Enter) emits `activated` — folders are entered, files previewed.
///
/// While a search filter is set, the list shows every matching file below the
/// current folder, grouped by subfolder.
class FolderView : public QWidget {
    Q_OBJECT
public:
    FolderView(VfsModel* model, QWidget* parent = nullptr);

    /// Shows the contents of `folder` (a container node, or null for nothing).
    void setFolder(const VfsNode* folder);
    [[nodiscard]] const VfsNode* folder() const noexcept { return m_folder; }
    /// Shown above the list while no folder is set.
    void setPlaceholder(const QString& title, const QString& text);

    void setFilterText(const QString& text);

    [[nodiscard]] std::vector<const VfsNode*> selectedNodes() const;
    [[nodiscard]] const VfsNode* nodeAt(const QPoint& viewportPos) const;
    /// Selects (and scrolls to) a node shown in the list.
    void selectNode(const VfsNode* node);
    void clearSelection();

    [[nodiscard]] QTreeView* view() const noexcept { return m_list; }
    [[nodiscard]] BackupBanner* banner() const noexcept { return m_banner; }

    [[nodiscard]] QByteArray saveHeaderState() const;
    void restoreHeaderState(const QByteArray& state);

signals:
    /// Double-click / Enter on an item.
    void activated(const VfsNode* node);
    /// The user picked a place in the path bar or pressed the Up button.
    void navigateRequested(const VfsNode* folder);
    void selectionChanged();
    void contextMenuRequested(const QPoint& globalPos);

private:
    void updateRoot();
    void updateHeader();
    void showHeaderMenu(const QPoint& pos);
    [[nodiscard]] const VfsNode* nodeFromProxy(const QModelIndex& index) const;

    VfsModel* m_model;
    VfsFilterModel* m_proxy;
    const VfsNode* m_folder = nullptr;
    std::vector<const VfsNode*> m_crumbs; ///< Path-bar targets (link "N" → m_crumbs[N]).
    QString m_placeholderTitle;
    QString m_placeholderText;
    QByteArray m_headerState; ///< Column layout carried across model resets.

    QToolButton* m_up;
    QLabel* m_title;
    QLabel* m_path;
    BackupBanner* m_banner;
    QStackedWidget* m_stack;
    QTreeView* m_list;
    QLabel* m_empty;
};

} // namespace viewer::app
