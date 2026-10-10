#pragma once

#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/Vfs.hpp"

#include <QAbstractItemModel>
#include <QIcon>
#include <QSortFilterProxyModel>

namespace viewer::app {

/// Exposes a Vfs tree to Qt views. Each QModelIndex stores a VfsNode* in its
/// internal pointer; the tree is immutable while the model is attached.
/// The root itself is the single top-level row (shown as the game's name).
class VfsModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Type, Size, Packed, Modified, ColumnCount };
    enum Role { NodeRole = Qt::UserRole + 1, SortRole };

    explicit VfsModel(QObject* parent = nullptr);

    /// Attaches a tree (or detaches with nullptrs). Both must outlive the model's use.
    /// `rootLabel` / `rootIcon` are shown for the root row.
    void setSource(const VfsNode* root, const ContentRegistry* content, const QString& rootLabel = {},
                   const QIcon& rootIcon = {});

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
    const VfsNode* m_root = nullptr;
    const ContentRegistry* m_content = nullptr;
    QString m_rootLabel;
    QIcon m_rootIcon;
};

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
