#include "FolderView.hpp"

#include "BackupBanner.hpp"
#include "Icons.hpp"
#include "VfsModel.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace viewer::app {

namespace {

/// Files below `parent` in the (filtered) model.
int countFiles(const QSortFilterProxyModel& model, const QModelIndex& parent)
{
    int files = 0;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        const VfsNode* node = VfsModel::nodeFromIndex(model.mapToSource(index));
        files += node && node->isFile() ? 1 : countFiles(model, index);
    }
    return files;
}

} // namespace

FolderView::FolderView(VfsModel* model, QWidget* parent)
    : QWidget(parent), m_model(model), m_proxy(new VfsFilterModel(this)), m_up(new QToolButton(this)),
      m_title(new QLabel(this)), m_path(new QLabel(this)), m_banner(new BackupBanner(this)),
      m_stack(new QStackedWidget(this)), m_list(new QTreeView(this)), m_empty(new QLabel(this))
{
    m_proxy->setSourceModel(model);

    // Header: [Up]  Title
    //               Game › cache › root.pak  ·  812 items
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long names must not widen the pane
    m_path->setTextFormat(Qt::RichText);
    m_path->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_path->setOpenExternalLinks(false);
    m_path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(m_path, &QLabel::linkActivated, this, [this](const QString& link) {
        const auto i = static_cast<std::size_t>(link.toUInt());
        if (i < m_crumbs.size())
            emit navigateRequested(m_crumbs[i]);
    });

    m_up->setIcon(icons::theme(QIcon::ThemeIcon::GoUp, QStyle::SP_FileDialogToParent));
    m_up->setIconSize(QSize(20, 20));
    m_up->setAutoRaise(true);
    m_up->setToolTip(tr("Up one level  (Backspace)"));
    connect(m_up, &QToolButton::clicked, this, [this] {
        if (m_folder && m_folder->parent())
            emit navigateRequested(m_folder->parent());
    });

    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_path);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(6);
    titleRow->addWidget(m_up, 0, Qt::AlignTop);
    titleRow->addLayout(titles, 1);

    auto* header = new QVBoxLayout;
    header->setContentsMargins(8, 10, 12, 6);
    header->setSpacing(2);
    header->addLayout(titleRow);
    header->addSpacing(6);
    header->addWidget(m_banner);

    // List
    m_list->setModel(m_proxy);
    m_list->setUniformRowHeights(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(VfsModel::Name, Qt::AscendingOrder);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAllColumnsShowFocus(true);
    m_list->setExpandsOnDoubleClick(false); // double-click enters folders / opens files instead
    m_list->setRootIsDecorated(false);
    m_list->setItemsExpandable(false);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    connect(m_list, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (const VfsNode* node = nodeFromProxy(index))
            emit activated(node);
    });
    connect(m_list, &QTreeView::customContextMenuRequested, this,
            [this](const QPoint& pos) { emit contextMenuRequested(m_list->viewport()->mapToGlobal(pos)); });
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderView::selectionChanged);

    QHeaderView* columns = m_list->header();
    columns->setStretchLastSection(false);
    columns->setMinimumSectionSize(60);
    columns->resizeSection(VfsModel::Type, 150);
    columns->resizeSection(VfsModel::Size, 80);
    columns->resizeSection(VfsModel::Packed, 80);
    columns->resizeSection(VfsModel::Modified, 140);
    columns->setSectionHidden(VfsModel::Modified, true); // available via the header menu
    columns->setSectionResizeMode(VfsModel::Name, QHeaderView::Stretch);
    columns->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(columns, &QHeaderView::customContextMenuRequested, this, &FolderView::showHeaderMenu);

    // QHeaderView forgets per-section resize modes on a model reset; the node
    // pointers die with the old session, so drop the folder too.
    connect(m_proxy, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        m_headerState = m_list->header()->saveState();
        m_folder = nullptr;
    });
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this] {
        restoreHeaderState(m_headerState);
        updateRoot();
    });

    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    m_stack->addWidget(m_list);
    m_stack->addWidget(m_empty);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_stack, 1);

    updateRoot();
}

const VfsNode* FolderView::nodeFromProxy(const QModelIndex& index) const
{
    return VfsModel::nodeFromIndex(m_proxy->mapToSource(index));
}

void FolderView::setFolder(const VfsNode* folder)
{
    m_folder = folder;
    clearSelection();
    updateRoot();
}

void FolderView::setPlaceholder(const QString& title, const QString& text)
{
    m_placeholderTitle = title;
    m_placeholderText = text;
    if (!m_folder)
        updateRoot();
}

void FolderView::setFilterText(const QString& text)
{
    if (text == m_proxy->filterRegularExpression().pattern())
        return;
    m_proxy->setFilterFixedString(text);
    updateRoot();
}

void FolderView::updateRoot()
{
    const bool searching = !m_proxy->filterRegularExpression().pattern().isEmpty();
    m_list->setRootIsDecorated(searching);
    m_list->setItemsExpandable(searching);

    const QModelIndex root = m_folder ? m_proxy->mapFromSource(m_model->indexForNode(m_folder)) : QModelIndex{};
    if (!m_folder) {
        m_empty->setText(m_placeholderText);
        m_stack->setCurrentWidget(m_empty);
    } else if (!root.isValid() || m_proxy->rowCount(root) == 0) {
        // A filtered-out folder has no matches below it.
        m_empty->setText(searching ? tr("No files here match “%1”.").arg(m_proxy->filterRegularExpression().pattern())
                                   : tr("This folder is empty."));
        m_stack->setCurrentWidget(m_empty);
    } else {
        m_list->setRootIndex(root);
        if (searching)
            m_list->expandAll();
        else
            m_list->collapseAll();
        m_stack->setCurrentWidget(m_list);
    }
    updateHeader();
}

void FolderView::updateHeader()
{
    m_crumbs.clear();
    m_up->setEnabled(m_folder && m_folder->parent());
    if (!m_folder) {
        m_title->setText(m_placeholderTitle);
        m_path->clear();
        return;
    }

    const auto label = [this](const VfsNode* node) {
        return m_model->data(m_model->indexForNode(node), Qt::DisplayRole).toString();
    };
    m_title->setText(label(m_folder));

    for (const VfsNode* n = m_folder; n; n = n->parent())
        m_crumbs.push_back(n);
    std::ranges::reverse(m_crumbs);
    QStringList parts;
    for (std::size_t i = 0; i < m_crumbs.size(); ++i) {
        const QString name = label(m_crumbs[i]).toHtmlEscaped();
        parts << (i + 1 < m_crumbs.size()
                      ? QStringLiteral("<a href=\"%1\" style=\"text-decoration:none\">%2</a>").arg(i).arg(name)
                      : name);
    }

    const QModelIndex root = m_proxy->mapFromSource(m_model->indexForNode(m_folder));
    QString count;
    if (!m_proxy->filterRegularExpression().pattern().isEmpty())
        count = tr("%n match(es)", nullptr, root.isValid() ? countFiles(*m_proxy, root) : 0);
    else
        count = tr("%n item(s)", nullptr, root.isValid() ? m_proxy->rowCount(root) : 0);
    m_path->setText(parts.join(QStringLiteral(" › ")) + QStringLiteral("&nbsp;&nbsp;·&nbsp;&nbsp;") + count);
}

std::vector<const VfsNode*> FolderView::selectedNodes() const
{
    std::vector<const VfsNode*> nodes;
    for (const QModelIndex& index : m_list->selectionModel()->selectedRows(VfsModel::Name))
        if (const VfsNode* node = nodeFromProxy(index))
            nodes.push_back(node);
    return nodes;
}

const VfsNode* FolderView::nodeAt(const QPoint& viewportPos) const
{
    return nodeFromProxy(m_list->indexAt(viewportPos));
}

void FolderView::selectNode(const VfsNode* node)
{
    const QModelIndex index = m_proxy->mapFromSource(m_model->indexForNode(node));
    if (!index.isValid())
        return;
    m_list->setCurrentIndex(index);
    m_list->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void FolderView::clearSelection()
{
    m_list->selectionModel()->clear();
}

QByteArray FolderView::saveHeaderState() const
{
    return m_list->header()->saveState();
}

void FolderView::restoreHeaderState(const QByteArray& state)
{
    QHeaderView* header = m_list->header();
    if (!state.isEmpty())
        header->restoreState(state);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(VfsModel::Name, QHeaderView::Stretch);
}

void FolderView::showHeaderMenu(const QPoint& pos)
{
    QHeaderView* header = m_list->header();
    QMenu menu(this);
    for (int column = VfsModel::Type; column < VfsModel::ColumnCount; ++column) {
        QAction* action = menu.addAction(m_model->headerData(column, Qt::Horizontal).toString());
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(column));
        connect(action, &QAction::toggled, header,
                [header, column](bool visible) { header->setSectionHidden(column, !visible); });
    }
    menu.exec(header->viewport()->mapToGlobal(pos));
}

} // namespace viewer::app
