#include "TextEditorWindow.hpp"

#include "Icons.hpp"
#include "QtConvert.hpp"
#include "preview/TextView.hpp"

#include "viewer/core/ContentRegistry.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>

namespace viewer::app {

namespace {

constexpr std::uint64_t kEditLimit = 16ull << 20;

} // namespace

TextEditorWindow::TextEditorWindow(QWidget* parent)
    : QMainWindow(parent), m_view(new TextView(this)), m_encoding(new QComboBox(this)),
      m_lineEnding(new QComboBox(this)), m_position(new QLabel(this))
{
    using TI = QIcon::ThemeIcon;
    setAttribute(Qt::WA_DeleteOnClose);
    setCentralWidget(m_view);
    m_view->setReadOnly(false);
    CodeEditor* editor = m_view->editor();

    m_saveAction = new QAction(icons::theme(TI::DocumentSave, QStyle::SP_DialogSaveButton), tr("&Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setStatusTip(tr("Write your changes back into the game (needs Edit Mode)"));
    connect(m_saveAction, &QAction::triggered, this, &TextEditorWindow::save);

    m_saveCopyAction =
        new QAction(icons::theme(TI::DocumentSaveAs, QStyle::SP_DialogSaveButton), tr("Save &Copy As…"), this);
    m_saveCopyAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    m_saveCopyAction->setStatusTip(tr("Save the text to a file of your choice (the game is not changed)"));
    connect(m_saveCopyAction, &QAction::triggered, this, &TextEditorWindow::saveCopy);

    m_undoAction = new QAction(icons::theme(TI::EditUndo, QStyle::SP_ArrowBack), tr("&Undo"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(false);
    connect(m_undoAction, &QAction::triggered, editor, &QPlainTextEdit::undo);
    connect(editor, &QPlainTextEdit::undoAvailable, m_undoAction, &QAction::setEnabled);

    m_redoAction = new QAction(icons::theme(TI::EditRedo, QStyle::SP_ArrowForward), tr("&Redo"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(false);
    connect(m_redoAction, &QAction::triggered, editor, &QPlainTextEdit::redo);
    connect(editor, &QPlainTextEdit::redoAvailable, m_redoAction, &QAction::setEnabled);

    auto* replace = new QAction(tr("Find && &Replace…"), this);
    connect(replace, &QAction::triggered, this, [this] { m_view->openFind(true); });

    m_revealAction = new QAction(icons::theme(TI::GoUp, QStyle::SP_FileDialogToParent), tr("Show in &List"), this);
    m_revealAction->setStatusTip(tr("Select this file in the main window"));
    connect(m_revealAction, &QAction::triggered, this, &TextEditorWindow::revealRequested);

    auto* close = new QAction(tr("Close"), this);
    close->setShortcuts({QKeySequence(QKeySequence::Close), QKeySequence(Qt::CTRL | Qt::Key_W)});
    connect(close, &QAction::triggered, this, &QWidget::close);
    addAction(close);

    QToolBar* toolbar = addToolBar(tr("Editor"));
    toolbar->setObjectName("editorToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(m_revealAction);
    toolbar->addSeparator();
    toolbar->addAction(m_saveAction);
    toolbar->addAction(m_saveCopyAction);
    toolbar->addSeparator();
    toolbar->addAction(m_undoAction);
    toolbar->addAction(m_redoAction);
    toolbar->addSeparator();
    toolbar->addAction(replace); // plain Find is on the text bar

    // Status bar: cursor position, and the encoding / line endings used when saving
    // (the file's own, unless the user picks others).
    for (TextEncoding e : {TextEncoding::Utf8, TextEncoding::Utf8Bom, TextEncoding::Utf16LE, TextEncoding::Utf16BE,
                           TextEncoding::Latin1})
        m_encoding->addItem(toQString(encodingName(e)), static_cast<int>(e));
    m_encoding->setToolTip(tr("Encoding used when saving"));
    m_lineEnding->addItem(tr("CRLF (Windows)"), static_cast<int>(LineEnding::CRLF));
    m_lineEnding->addItem(tr("LF (Unix)"), static_cast<int>(LineEnding::LF));
    m_lineEnding->setToolTip(tr("Line endings used when saving"));
    for (QComboBox* combo : {m_encoding, m_lineEnding})
        connect(combo, &QComboBox::activated, this, [this] {
            m_view->editor()->document()->setModified(true); // saving now changes the file
        });
    statusBar()->addPermanentWidget(m_position);
    statusBar()->addPermanentWidget(m_encoding);
    statusBar()->addPermanentWidget(m_lineEnding);

    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, &TextEditorWindow::updatePosition);
    connect(editor->document(), &QTextDocument::modificationChanged, this, [this] {
        updateTitle();
        updateActions();
    });

    if (!restoreGeometry(QSettings().value("editor/geometry").toByteArray()))
        resize(1000, 760);
}

TextEditorWindow::~TextEditorWindow() = default;

QString TextEditorWindow::open(std::shared_ptr<Session> session, const VfsNode* node, const QString& location)
{
    if (!session || !node || !node->isFile())
        return tr("Nothing to edit.");
    if (node->size() > kEditLimit)
        return tr("%1 is too large to edit here (%2).")
            .arg(toQString(node->name()), QLocale().formattedDataSize(static_cast<qint64>(node->size())));
    auto bytes = session->vfs.read(*node);
    if (!bytes)
        return toQString(bytes.error().message);
    if (!bytes->empty() && !looksLikeText(*bytes))
        return tr("%1 doesn't look like a text file.").arg(toQString(node->name()));
    auto doc = decodeText(*bytes);
    if (!doc)
        return tr("Can't read %1 as text: %2").arg(toQString(node->name()), toQString(doc.error().message));

    m_session = std::move(session);
    m_node = node;
    m_location = location;
    m_name = toQString(node->name());
    m_view->setText(toQString(doc->text), syntaxFor(node->name(), doc->text),
                    QStringLiteral("%1  ·  %2").arg(toQString(node->virtualPath()), toQString(encodingName(doc->encoding))));
    m_encoding->setCurrentIndex(m_encoding->findData(static_cast<int>(doc->encoding)));
    m_lineEnding->setCurrentIndex(m_lineEnding->findData(static_cast<int>(doc->lineEnding)));
    m_view->editor()->document()->setModified(false);
    m_view->editor()->document()->clearUndoRedoStacks();
    updateTitle();
    updateActions();
    updatePosition();
    return {};
}

void TextEditorWindow::rebind(std::shared_ptr<Session> session, const VfsNode* node)
{
    m_session = node ? std::move(session) : nullptr;
    m_node = node;
    if (!node && !m_location.isEmpty())
        statusBar()->showMessage(tr("This file is no longer open in 26Viewer — use Save Copy As to keep your text."));
    updateTitle();
    updateActions();
}

bool TextEditorWindow::isModified() const
{
    return m_view->editor()->document()->isModified();
}

Result<ByteBuffer> TextEditorWindow::encoded() const
{
    // toRawText keeps every character as typed (toPlainText would turn
    // non-breaking spaces into spaces); blocks are separated by U+2029.
    QString text = m_view->editor()->document()->toRawText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    TextDocument doc;
    doc.text = text.toStdString();
    doc.encoding = static_cast<TextEncoding>(m_encoding->currentData().toInt());
    doc.lineEnding = static_cast<LineEnding>(m_lineEnding->currentData().toInt());
    return encodeText(doc);
}

void TextEditorWindow::save()
{
    if (!m_node || m_saving)
        return;
    auto bytes = encoded();
    if (!bytes) {
        QMessageBox::warning(this, tr("Save"),
                             tr("Can't save in %1: %2.\n\nPick another encoding in the status bar (e.g. UTF-8).")
                                 .arg(m_encoding->currentText(), toQString(bytes.error().message)));
        m_closeAfterSave = false;
        return;
    }
    m_saving = true;
    updateActions();
    emit saveRequested(QByteArray(reinterpret_cast<const char*>(bytes->data()), static_cast<qsizetype>(bytes->size())));
}

void TextEditorWindow::saveFinished(bool ok)
{
    m_saving = false;
    if (ok) {
        m_view->editor()->document()->setModified(false);
        statusBar()->showMessage(tr("Saved into the game"), 5000);
    }
    updateActions();
    if (std::exchange(m_closeAfterSave, false) && ok)
        close();
}

void TextEditorWindow::saveCopy()
{
    auto bytes = encoded();
    if (!bytes) {
        QMessageBox::warning(this, tr("Save Copy As"), toQString(bytes.error().message));
        return;
    }
    QSettings settings;
    const QString dir = settings.value("paths/lastTextCopyDir").toString();
    const QString file =
        QFileDialog::getSaveFileName(this, tr("Save a copy of %1").arg(m_name), dir.isEmpty() ? m_name : dir + '/' + m_name);
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastTextCopyDir", QFileInfo(file).absolutePath());
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<qint64>(bytes->size()))
               != static_cast<qint64>(bytes->size())) {
        QMessageBox::warning(this, tr("Save Copy As"), tr("Could not write %1:\n%2").arg(file, out.errorString()));
        return;
    }
    statusBar()->showMessage(tr("Saved a copy to %1").arg(QDir::toNativeSeparators(file)), 8000);
}

void TextEditorWindow::updateTitle()
{
    setWindowTitle(QStringLiteral("%1[*] — %2 — %3")
                       .arg(m_name.isEmpty() ? tr("Text") : m_name, tr("Edit"), QApplication::applicationDisplayName()));
    setWindowModified(isModified());
}

void TextEditorWindow::updateActions()
{
    m_saveAction->setEnabled(m_node && !m_saving && Vfs::canReplace(*m_node));
    m_revealAction->setEnabled(m_node != nullptr);
}

void TextEditorWindow::updatePosition()
{
    const QTextCursor c = m_view->editor()->textCursor();
    m_position->setText(tr("Ln %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
}

void TextEditorWindow::closeEvent(QCloseEvent* event)
{
    if (m_saving) { // wait for the save that is running
        m_closeAfterSave = true;
        event->ignore();
        return;
    }
    if (isModified()) {
        QMessageBox box(QMessageBox::Question, tr("Unsaved changes"),
                        tr("Save your changes to %1?").arg(m_name.toHtmlEscaped()), QMessageBox::NoButton, this);
        QPushButton* saveButton = m_saveAction->isEnabled() ? box.addButton(tr("Save"), QMessageBox::AcceptRole) : nullptr;
        QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (saveButton && box.clickedButton() == saveButton) {
            m_closeAfterSave = true; // closes once the save went through
            save();
            event->ignore();
            return;
        }
        if (box.clickedButton() != discard) {
            event->ignore();
            return;
        }
    }
    if (!testAttribute(Qt::WA_DontShowOnScreen))
        QSettings().setValue("editor/geometry", saveGeometry());
    QMainWindow::closeEvent(event);
}

} // namespace viewer::app
