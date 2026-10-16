#include "HashWindow.hpp"

#include "QtConvert.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace viewer::app {

HashWindow::HashWindow(QWidget* parent)
    : QDialog(parent), m_about(new QLabel(this)), m_query(new QLineEdit(this)), m_results(new QTreeWidget(this)),
      m_copy(new QPushButton(tr("&Copy Value"), this)), m_typing(new QTimer(this))
{
    setWindowTitle(tr("Hashes"));
    m_about->setWordWrap(true);
    m_about->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_query->setPlaceholderText(tr("A name, a text or a number (0x…)"));
    m_query->setClearButtonEnabled(true);
    m_results->setColumnCount(3);
    m_results->setHeaderLabels({tr("What"), tr("Value"), tr("Details")});
    m_results->setRootIsDecorated(false);
    m_results->setAllColumnsShowFocus(true);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_results->header()->setStretchLastSection(true);
    m_copy->setEnabled(false);
    m_copy->setToolTip(tr("Copy the selected value (double-click a row does the same)"));

    // Look up once typing pauses (reading the game's text tables takes a moment).
    m_typing->setSingleShot(true);
    m_typing->setInterval(250);
    connect(m_typing, &QTimer::timeout, this, &HashWindow::lookup);
    connect(m_query, &QLineEdit::textChanged, m_typing, qOverload<>(&QTimer::start));
    connect(m_query, &QLineEdit::returnPressed, this, &HashWindow::lookup);
    connect(m_results, &QTreeWidget::itemSelectionChanged, this,
            [this] { m_copy->setEnabled(!m_results->selectedItems().isEmpty()); });
    connect(m_results, &QTreeWidget::itemDoubleClicked, this, &HashWindow::copySelected);
    connect(m_copy, &QPushButton::clicked, this, &HashWindow::copySelected);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(m_copy, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_about);
    layout->addWidget(m_query);
    layout->addWidget(m_results, 1);
    layout->addWidget(buttons);
    resize(760, 360);
    setSession(nullptr);
}

void HashWindow::setSession(std::shared_ptr<Session> session)
{
    m_session = std::move(session);
    const HashTool* tool = m_session ? m_session->content.hashTool() : nullptr;
    m_about->setText(tool ? toQString(tool->description())
                          : m_session ? tr("This game has no names or hashes 26Viewer knows.")
                                      : tr("Open a game to look up its names and hashes."));
    m_query->setEnabled(tool != nullptr);
    lookup();
}

void HashWindow::search(const QString& text)
{
    m_query->setText(text);
    lookup();
}

void HashWindow::lookup()
{
    m_typing->stop();
    m_results->clear();
    const HashTool* tool = m_session ? m_session->content.hashTool() : nullptr;
    const std::string query = m_query->text().toStdString(); // UTF-8
    if (!tool || query.empty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto matches = tool->lookup(query, m_session->vfs.hashToolContext());
    QApplication::restoreOverrideCursor();
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const HashMatch& m : matches) {
        auto* item = new QTreeWidgetItem(m_results, {toQString(m.kind), toQString(m.value), toQString(m.note)});
        item->setFont(1, fixed);
        item->setToolTip(2, toQString(m.note));
    }
    if (m_results->topLevelItemCount())
        m_results->setCurrentItem(m_results->topLevelItem(0));
}

void HashWindow::copySelected()
{
    const auto items = m_results->selectedItems();
    if (!items.isEmpty())
        QApplication::clipboard()->setText(items.front()->text(1));
}

} // namespace viewer::app
