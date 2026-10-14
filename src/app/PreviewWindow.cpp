#include "PreviewWindow.hpp"

#include "Exporter.hpp"
#include "Icons.hpp"
#include "QtConvert.hpp"
#include "TextureBrowser.hpp"
#include "preview/PreviewPane.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

namespace viewer::app {

PreviewWindow::PreviewWindow(QWidget* parent) : QMainWindow(parent), m_pane(new PreviewPane(this))
{
    using TI = QIcon::ThemeIcon;
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    setCentralWidget(m_pane);
    connect(m_pane, &PreviewPane::backupRequested, this, &PreviewWindow::backupRequested);
    connect(m_pane, &PreviewPane::editRequested, this, &PreviewWindow::editRequested);
    connect(m_pane, &PreviewPane::replaceSoundRequested, this, &PreviewWindow::replaceSoundRequested);

    m_revealAction = new QAction(icons::theme(TI::GoUp, QStyle::SP_FileDialogToParent), tr("Show in &List"), this);
    m_revealAction->setStatusTip(tr("Select this file in the main window"));
    connect(m_revealAction, &QAction::triggered, this, &PreviewWindow::revealRequested);

    m_extractAction =
        new QAction(icons::theme(TI::DocumentSaveAs, QStyle::SP_DialogSaveButton), tr("&Extract…"), this);
    m_extractAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(m_extractAction, &QAction::triggered, this, &PreviewWindow::extractRequested);

    m_replaceAction = new QAction(icons::theme(TI::DocumentRevert, QStyle::SP_BrowserReload), tr("&Replace…"), this);
    m_replaceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    m_replaceAction->setStatusTip(tr("Replace this file with one of your own (or drop a file on this window). "
                                     "Needs Edit Mode."));
    m_replaceAction->setEnabled(false);
    connect(m_replaceAction, &QAction::triggered, this, [this] { emit replaceRequested(QString()); });

    m_exportAction = new QAction(icons::theme(TI::DocumentSend, QStyle::SP_DialogSaveButton), tr("E&xport…"), this);
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_E));
    m_exportAction->setStatusTip(tr("Save this file converted for other programs (sounds → .wav, models → .obj, "
                                    "textures → .png / .dds / .jpg / .bmp)"));
    connect(m_exportAction, &QAction::triggered, this, &PreviewWindow::exportRequested);

    m_importAction = new QAction(icons::theme(TI::DocumentOpen, QStyle::SP_DialogOpenButton), tr("&Import…"), this);
    m_importAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_importAction->setStatusTip(tr("Convert your own files back into this one (a folder of .wav files into a "
                                    "sound bank, a picture into a texture). Needs Edit Mode."));
    m_importAction->setEnabled(false);
    connect(m_importAction, &QAction::triggered, this, &PreviewWindow::importRequested);

    m_texturesAction = new QAction(icons::forCategory(FileCategory::Texture), tr("&Textures…"), this);
    m_texturesAction->setStatusTip(tr("The textures inside this file: browse, export, extract or replace them"));
    m_texturesAction->setVisible(false);
    connect(m_texturesAction, &QAction::triggered, this, &PreviewWindow::showTextures);

    auto* close = new QAction(tr("Close"), this);
    close->setShortcuts({QKeySequence(QKeySequence::Close), QKeySequence(Qt::CTRL | Qt::Key_W),
                         QKeySequence(Qt::Key_Escape)});
    connect(close, &QAction::triggered, this, &QWidget::close);
    addAction(close);

    // Per-file tools go on this toolbar (image adjustments, export, ...).
    QToolBar* toolbar = addToolBar(tr("Preview"));
    toolbar->setObjectName("previewToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(m_revealAction);
    toolbar->addSeparator();
    toolbar->addAction(m_extractAction);
    toolbar->addAction(m_exportAction);
    toolbar->addAction(m_replaceAction);
    toolbar->addAction(m_importAction);
    toolbar->addSeparator();
    toolbar->addAction(m_texturesAction);

    statusBar(); // shows status tips and drag & drop feedback
    if (!restoreGeometry(QSettings().value("preview/geometry").toByteArray()))
        resize(1000, 760);
}

PreviewWindow::~PreviewWindow() = default;

void PreviewWindow::setSession(std::shared_ptr<Session> session)
{
    m_pane->setSession(std::move(session));
}

void PreviewWindow::showNode(const VfsNode* node, bool properties)
{
    if (properties)
        m_pane->showProperties(node);
    else
        m_pane->showNode(node);
    m_extractAction->setEnabled(node != nullptr);
    m_exportAction->setEnabled(node != nullptr);
    m_revealAction->setEnabled(node != nullptr);
    m_texturesAction->setVisible(node && m_pane->session() && textureDictionary(*m_pane->session(), *node));
    setEditMode(m_editMode);
    updateTitle();
    // An open Textures window follows (after a replace the file is read again).
    if (TextureBrowser* b = textureBrowser()) {
        if (node)
            b->setSource(m_pane->session(), node);
        else
            b->close();
    }
}

TextureBrowser* PreviewWindow::textureBrowser() const
{
    return findChild<TextureBrowser*>(QString(), Qt::FindDirectChildrenOnly);
}

void PreviewWindow::showTextures()
{
    TextureBrowser* b = textureBrowser();
    if (!b) {
        b = new TextureBrowser(this);
        if (testAttribute(Qt::WA_DontShowOnScreen))
            b->setAttribute(Qt::WA_DontShowOnScreen);
        connect(b, &TextureBrowser::replaceRequested, this, &PreviewWindow::replaceTextureRequested);
        connect(b, &TextureBrowser::importRequested, this, &PreviewWindow::importTexturesRequested);
        b->setEditMode(m_editMode);
        b->setSource(m_pane->session(), node());
    }
    b->show();
    b->raise();
    b->activateWindow();
}

const VfsNode* PreviewWindow::node() const
{
    return m_pane->node();
}

void PreviewWindow::setEditMode(bool on)
{
    m_editMode = on;
    const VfsNode* n = node();
    m_replaceAction->setEnabled(on && n && Vfs::canReplace(*n));
    m_importAction->setEnabled(on && n && m_importCheck && m_importCheck(n));
    if (TextureBrowser* b = textureBrowser())
        b->setEditMode(on);
}

void PreviewWindow::setBackupWarning(const QString& fileName)
{
    m_pane->setBackupWarning(fileName);
}

void PreviewWindow::setBackupInProgress(bool running)
{
    m_pane->setBackupInProgress(running);
}

void PreviewWindow::updateTitle()
{
    const VfsNode* n = node();
    const QString name = n ? toQString(n->name()) : QFileInfo(m_location).fileName();
    setWindowTitle(QStringLiteral("%1 — %2").arg(name.isEmpty() ? tr("Preview") : name,
                                                 QApplication::applicationDisplayName()));
}

void PreviewWindow::closeEvent(QCloseEvent* event)
{
    if (!testAttribute(Qt::WA_DontShowOnScreen)) // automation runs don't change the user's layout
        QSettings().setValue("preview/geometry", saveGeometry());
    QMainWindow::closeEvent(event);
}

// ---- Drag & drop: a file dropped on the window replaces the file it shows -----------------

QString PreviewWindow::droppedFile(const QMimeData* mime) const
{
    if (!mime || !mime->hasUrls() || mime->urls().size() != 1 || !mime->urls().front().isLocalFile())
        return {};
    const QString file = mime->urls().front().toLocalFile();
    return QFileInfo(file).isFile() ? file : QString();
}

void PreviewWindow::dragEnterEvent(QDragEnterEvent* event)
{
    dragMoveEvent(event);
}

void PreviewWindow::dragMoveEvent(QDragMoveEvent* event)
{
    const QString file = droppedFile(event->mimeData());
    if (file.isEmpty())
        return;
    const QString reason = m_dropCheck ? m_dropCheck(file) : tr("Replacing isn't available here");
    if (reason.isEmpty()) {
        event->acceptProposedAction();
        statusBar()->showMessage(tr("Drop to replace %1").arg(node() ? toQString(node()->name()) : QString()));
    } else {
        event->setDropAction(Qt::IgnoreAction); // "not allowed" cursor
        event->accept();
        statusBar()->showMessage(reason);
    }
}

void PreviewWindow::dropEvent(QDropEvent* event)
{
    const QString file = droppedFile(event->mimeData());
    if (file.isEmpty())
        return;
    statusBar()->clearMessage();
    const QString reason = m_dropCheck ? m_dropCheck(file) : tr("Replacing isn't available here");
    event->acceptProposedAction();
    // Act after the drop completes so Explorer isn't blocked by our dialogs.
    if (reason.isEmpty())
        QTimer::singleShot(0, this, [this, file] { emit replaceRequested(file); });
    else
        QTimer::singleShot(0, this, [this, reason] { QMessageBox::information(this, tr("Replace"), reason); });
}

} // namespace viewer::app
