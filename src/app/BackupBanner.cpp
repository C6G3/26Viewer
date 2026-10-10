#include "BackupBanner.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace viewer::app {

BackupBanner::BackupBanner(QWidget* parent)
    : QFrame(parent), m_text(new QLabel(this)), m_button(new QPushButton(tr("Back Up Now"), this))
{
    // A fixed red reads as a warning in both light and dark themes, so it
    // deliberately ignores the palette.
    setObjectName(QStringLiteral("backupBanner"));
    setStyleSheet(QStringLiteral(
        "QFrame#backupBanner { background: #C42B1C; border-radius: 6px; }"
        "QFrame#backupBanner QLabel { color: white; }"
        "QFrame#backupBanner QPushButton { background: white; color: #C42B1C; border: none;"
        "  border-radius: 4px; padding: 5px 14px; font-weight: 600; }"
        "QFrame#backupBanner QPushButton:hover { background: #FDE7E9; }"
        "QFrame#backupBanner QPushButton:disabled { background: #F1B9B3; color: #7A1A10; }"));
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::RichText);
    m_button->setCursor(Qt::PointingHandCursor);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 8, 8);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_button);
    hide();
    connect(m_button, &QPushButton::clicked, this, &BackupBanner::backupRequested);
}

void BackupBanner::setWarning(const QString& fileName)
{
    if (fileName.isEmpty()) {
        hide();
        return;
    }
    m_text->setText(tr("<b>⚠ %1 is not backed up.</b><br>Changes made in Edit Mode can't be undone "
                       "without a backup.")
                        .arg(fileName.toHtmlEscaped()));
    show();
}

void BackupBanner::setInProgress(bool running)
{
    m_button->setEnabled(!running);
    m_button->setText(running ? tr("Backing up…") : tr("Back Up Now"));
}

} // namespace viewer::app
