#pragma once

#include "Session.hpp"

#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/Vfs.hpp"

#include <QAbstractItemModel>
#include <QIcon>
#include <QSortFilterProxyModel>

#include <vector>

namespace viewer::app {

/// Exposes Vfs trees to Qt views. Each QModelIndex stores a VfsNode* in its
/// internal pointer; the trees are immutable while attached.
/// The top-level rows are the session's mounts: the game (or archive/folder)
/// first, then anything opened alongside it.
class VfsModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Type, Size, Packed, Modified, ColumnCount };
    enum Role { NodeRole = Qt::UserRole + 1, SortRole };

    /// A top-level row.
    struct Root {
        const VfsNode* node = nullptr;
        QString label;
        QString tooltip;
        QIcon icon;
    };

    explicit VfsModel(QObject* parent = nullptr);

    /// Replaces all top-level rows (empty to detach). The nodes and `content`
    /// must outlive their use by the model.
    void setRoots(std::vector<Root> roots, const ContentRegistry* content);
    void appendRoot(Root root);
    void removeRoot(int row);
    [[nodiscard]] int rootCount() const noexcept { return static_cast<int>(m_roots.size()); }
    /// Row of a top-level node, or -1.
    [[nodiscard]] int rootRow(const VfsNode* node) const;
    [[nodiscard]] bool isRoot(const VfsNode* node) const { return rootRow(node) >= 0; }
    /// The top-level node `node` lives under (null if it isn't shown).
    [[nodiscard]] const VfsNode* topOf(const VfsNode* node) const;

    [[nodiscard]] static const VfsNode* nodeFromIndex(const QModelIndex& index);
    [[nodiscard]] QModelIndex indexForNode(const VfsNode* node, int column = Name) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    std::vector<Root> m_roots;
    const ContentRegistry* m_content = nullptr;
};

/// The tree's top row for a session's main mount (the game with its icon) and
/// for something opened alongside it.
[[nodiscard]] VfsModel::Root mainRoot(const Session& session);
[[nodiscard]] VfsModel::Root extraRoot(const Session::Extra& extra);

/// Sorting (folders first) and recursive name filtering.
class VfsFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit VfsFilterModel(QObject* parent = nullptr);

    /// Hides files, leaving folders and archives (the navigation tree).
    void setContainersOnly(bool on);

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    bool m_containersOnly = false;
};

} // namespace viewer::app
