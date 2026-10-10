#include "VfsModel.hpp"

#include "Icons.hpp"
#include "QtConvert.hpp"

#include <QDateTime>
#include <QLocale>
#include <QMimeData>

namespace viewer::app {

namespace {

QString sizeText(std::uint64_t bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

VfsModel::VfsModel(QObject* parent) : QAbstractItemModel(parent) {}

void VfsModel::setSource(const VfsNode* root, const ContentRegistry* content)
{
    beginResetModel();
    m_root = root;
    m_content = content;
    endResetModel();
}

const VfsNode* VfsModel::nodeFromIndex(const QModelIndex& index)
{
    return index.isValid() ? static_cast<const VfsNode*>(index.internalPointer()) : nullptr;
}

QModelIndex VfsModel::indexForNode(const VfsNode* node, int column) const
{
    if (!node || node == m_root || !node->parent())
        return {};
    return createIndex(static_cast<int>(node->row()), column, const_cast<VfsNode*>(node));
}

QModelIndex VfsModel::index(int row, int column, const QModelIndex& parent) const
{
    const VfsNode* p = parent.isValid() ? nodeFromIndex(parent) : m_root;
    if (!p || row < 0 || column < 0 || column >= ColumnCount || static_cast<std::size_t>(row) >= p->children().size())
        return {};
    return createIndex(row, column, const_cast<VfsNode*>(p->children()[static_cast<std::size_t>(row)].get()));
}

QModelIndex VfsModel::parent(const QModelIndex& child) const
{
    const VfsNode* node = nodeFromIndex(child);
    if (!node || !node->parent() || node->parent() == m_root)
        return {};
    const VfsNode* p = node->parent();
    return createIndex(static_cast<int>(p->row()), 0, const_cast<VfsNode*>(p));
}

int VfsModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0)
        return 0;
    const VfsNode* p = parent.isValid() ? nodeFromIndex(parent) : m_root;
    return p ? static_cast<int>(p->children().size()) : 0;
}

int VfsModel::columnCount(const QModelIndex&) const
{
    return ColumnCount;
}

QVariant VfsModel::data(const QModelIndex& index, int role) const
{
    const VfsNode* node = nodeFromIndex(index);
    if (!node)
        return {};

    if (role == NodeRole)
        return QVariant::fromValue(static_cast<const void*>(node));

    const auto typeInfo = [&] {
        return m_content ? m_content->fileTypes().lookup(node->name()) : FileTypeInfo{};
    };

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Name: return toQString(node->name());
        case Type:
            if (node->kind() == VfsNode::Kind::Folder)
                return tr("Folder");
            if (node->kind() == VfsNode::Kind::Archive)
                return tr("%1 (%Ln file(s))", nullptr, static_cast<int>(node->fileCount()))
                    .arg(toQString(node->archive() ? node->archive()->formatName() : "Archive"));
            return toQString(typeInfo().description);
        case Size: return sizeText(node->size());
        case Packed:
            return node->packedSize() == node->size() && node->kind() == VfsNode::Kind::File && !node->archive()
                       ? QVariant{}
                       : QVariant(sizeText(node->packedSize()));
        case Modified:
            if (const auto& t = node->modified()) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t->time_since_epoch());
                return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms.count()), QLocale::ShortFormat);
            }
            return {};
        }
        break;
    case SortRole:
        switch (index.column()) {
        case Name: return toQString(node->name()).toLower();
        case Type: return data(index, Qt::DisplayRole);
        case Size: return QVariant::fromValue<qulonglong>(node->size());
        case Packed: return QVariant::fromValue<qulonglong>(node->packedSize());
        case Modified:
            return node->modified() ? QVariant::fromValue<qint64>(node->modified()->time_since_epoch().count())
                                    : QVariant::fromValue<qint64>(0);
        }
        break;
    case Qt::DecorationRole:
        if (index.column() != Name)
            return {};
        switch (node->kind()) {
        case VfsNode::Kind::Folder: return icons::folder();
        case VfsNode::Kind::Archive: return icons::archive();
        case VfsNode::Kind::File: return icons::forCategory(typeInfo().category);
        }
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == Size || index.column() == Packed)
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        break;
    case Qt::ToolTipRole:
        if (index.column() == Name)
            return toQString(node->virtualPath());
        break;
    }
    return {};
}

QVariant VfsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return tr("Name");
    case Type: return tr("Type");
    case Size: return tr("Size");
    case Packed: return tr("Packed");
    case Modified: return tr("Modified");
    }
    return {};
}

Qt::ItemFlags VfsModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList VfsModel::mimeTypes() const
{
    return {QStringLiteral("text/plain")};
}

QMimeData* VfsModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList paths;
    for (const auto& index : indexes)
        if (index.column() == Name)
            if (const VfsNode* node = nodeFromIndex(index))
                paths << toQString(node->virtualPath());
    auto* mime = new QMimeData();
    mime->setText(paths.join('\n'));
    return mime;
}

// ---- Filter / sort proxy ----------------------------------------------------------------

VfsFilterModel::VfsFilterModel(QObject* parent) : QSortFilterProxyModel(parent)
{
    setSortRole(VfsModel::SortRole);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setRecursiveFilteringEnabled(true); // keep parents of matching files visible
    setAutoAcceptChildRows(false);
}

bool VfsFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const VfsNode* a = VfsModel::nodeFromIndex(left);
    const VfsNode* b = VfsModel::nodeFromIndex(right);
    if (a && b && a->isContainer() != b->isContainer()) {
        // Folders stay on top in both sort directions.
        const bool ascending = sortOrder() == Qt::AscendingOrder;
        return ascending ? a->isContainer() : b->isContainer();
    }
    return QSortFilterProxyModel::lessThan(left, right);
}

bool VfsFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QString pattern = filterRegularExpression().pattern();
    if (pattern.isEmpty())
        return true;
    const QModelIndex index = sourceModel()->index(sourceRow, VfsModel::Name, sourceParent);
    const VfsNode* node = VfsModel::nodeFromIndex(index);
    // Only files are matched directly; folders appear when a descendant matches.
    return node && node->isFile() && filterRegularExpression().match(toQString(node->name())).hasMatch();
}

} // namespace viewer::app
