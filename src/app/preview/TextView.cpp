#include "TextView.hpp"

#include "../Icons.hpp"
#include "../QtConvert.hpp"
#include "SyntaxHighlighter.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QSettings>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>

namespace viewer::app {

namespace {

/// Above this size colour coding is switched off (QSyntaxHighlighter works line by line on the GUI thread).
constexpr qsizetype kHighlightLimit = 2 << 20;

class Gutter : public QWidget {
public:
    explicit Gutter(CodeEditor* editor) : QWidget(editor), m_editor(editor) {}
    QSize sizeHint() const override { return {m_editor->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { m_editor->paintGutter(event); }

private:
    CodeEditor* m_editor;
};

QToolButton* barButton(const QIcon& icon, const QString& text, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setAutoRaise(true);
    return b;
}

} // namespace

// ---- CodeEditor -----------------------------------------------------------------------------

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent), m_gutter(new Gutter(this))
{
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), mono.family()});
    setFont(mono);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setAcceptDrops(false); // dropped files go to the window (replace)

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateGutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

int CodeEditor::gutterWidth() const
{
    int digits = 1;
    for (int max = std::max(1, blockCount()); max >= 10; max /= 10)
        ++digits;
    return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(digits, 3);
}

void CodeEditor::updateGutter(const QRect& rect, int dy)
{
    if (dy)
        m_gutter->scroll(0, dy);
    else
        m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
}

void CodeEditor::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect r = contentsRect();
    m_gutter->setGeometry(QRect(r.left(), r.top(), gutterWidth(), r.height()));
}

void CodeEditor::changeEvent(QEvent* event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ReadOnlyChange)
        highlightCurrentLine();
}

void CodeEditor::highlightCurrentLine()
{
    // A faint band on the cursor's line while editing.
    QList<QTextEdit::ExtraSelection> selections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection line;
        QColor band = palette().color(QPalette::Highlight);
        band.setAlpha(28);
        line.format.setBackground(band);
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        selections.append(line);
    }
    setExtraSelections(selections);
    m_gutter->update();
}

void CodeEditor::paintGutter(QPaintEvent* event)
{
    QPainter painter(m_gutter);
    const QPalette& pal = palette();
    painter.fillRect(event->rect(), pal.color(QPalette::Window));
    painter.setFont(font());

    const int current = textCursor().blockNumber();
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top()) {
            painter.setPen(pal.color(block.blockNumber() == current ? QPalette::Text : QPalette::PlaceholderText));
            painter.drawText(0, top, m_gutter->width() - 8, fontMetrics().height(), Qt::AlignRight,
                             QString::number(block.blockNumber() + 1));
        }
        block = block.next();
        top += height;
    }
}

// ---- FindBar --------------------------------------------------------------------------------

FindBar::FindBar(CodeEditor* editor, QWidget* parent)
    : QWidget(parent), m_editor(editor), m_find(new QLineEdit(this)), m_replace(new QLineEdit(this)),
      m_matchCase(new QCheckBox(tr("Match case"), this)), m_wholeWords(new QCheckBox(tr("Whole words"), this)),
      m_replaceRow(new QWidget(this)), m_status(new QLabel(this))
{
    using TI = QIcon::ThemeIcon;
    m_find->setPlaceholderText(tr("Find"));
    m_find->setClearButtonEnabled(true);
    m_replace->setPlaceholderText(tr("Replace with"));
    m_status->setForegroundRole(QPalette::PlaceholderText);

    auto* previous = barButton(icons::theme(TI::GoUp, QStyle::SP_ArrowUp), QString(), this);
    previous->setToolTip(tr("Previous match (Shift+Enter)"));
    auto* next = barButton(icons::theme(TI::GoDown, QStyle::SP_ArrowDown), QString(), this);
    next->setToolTip(tr("Next match (Enter)"));
    auto* close = barButton(icons::theme(TI::WindowClose, QStyle::SP_DialogCloseButton), QString(), this);
    close->setToolTip(tr("Close (Esc)"));
    auto* replaceOne = new QToolButton(this);
    replaceOne->setText(tr("Replace"));
    auto* replaceAll = new QToolButton(this);
    replaceAll->setText(tr("Replace All"));

    connect(previous, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        m_editor->setFocus();
    });
    connect(m_find, &QLineEdit::textChanged, this, [this] {
        // Search as you type, from where the current match starts.
        QTextCursor c = m_editor->textCursor();
        c.setPosition(c.selectionStart());
        m_editor->setTextCursor(c);
        findNext(false);
    });
    connect(replaceOne, &QToolButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAll, &QToolButton::clicked, this, &FindBar::replaceAll);
    m_find->installEventFilter(this);
    m_replace->installEventFilter(this);

    auto* findRow = new QHBoxLayout;
    findRow->addWidget(m_find, 1);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(m_matchCase);
    findRow->addWidget(m_wholeWords);
    findRow->addWidget(m_status);
    findRow->addStretch();
    findRow->addWidget(close);

    auto* replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->addWidget(m_replace, 1);
    replaceRow->addWidget(replaceOne);
    replaceRow->addWidget(replaceAll);
    replaceRow->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(4);
    layout->addLayout(findRow);
    layout->addWidget(m_replaceRow);
    hide();
}

void FindBar::open(bool replace)
{
    replace = replace && !m_editor->isReadOnly();
    m_replaceRow->setVisible(replace);
    if (const QString selected = m_editor->textCursor().selectedText();
        !selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator))
        m_find->setText(selected);
    show();
    m_find->setFocus();
    m_find->selectAll();
}

QTextDocument::FindFlags FindBar::flags(bool backwards) const
{
    QTextDocument::FindFlags f;
    if (backwards)
        f |= QTextDocument::FindBackward;
    if (m_matchCase->isChecked())
        f |= QTextDocument::FindCaseSensitively;
    if (m_wholeWords->isChecked())
        f |= QTextDocument::FindWholeWords;
    return f;
}

bool FindBar::findNext(bool backwards)
{
    const QString needle = m_find->text();
    m_status->clear();
    if (needle.isEmpty())
        return false;
    if (m_editor->find(needle, flags(backwards)))
        return true;
    // Wrap around once.
    QTextCursor c = m_editor->textCursor();
    c.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
    m_editor->setTextCursor(c);
    if (m_editor->find(needle, flags(backwards))) {
        m_status->setText(tr("Wrapped around"));
        return true;
    }
    m_status->setText(tr("Not found"));
    return false;
}

void FindBar::replaceOne()
{
    if (m_editor->isReadOnly())
        return;
    const QTextCursor c = m_editor->textCursor();
    const Qt::CaseSensitivity cs = m_matchCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (c.hasSelection() && c.selectedText().compare(m_find->text(), cs) == 0)
        m_editor->textCursor().insertText(m_replace->text());
    findNext(false);
}

void FindBar::replaceAll()
{
    if (m_editor->isReadOnly() || m_find->text().isEmpty())
        return;
    QTextCursor c = m_editor->textCursor();
    c.beginEditBlock(); // one undo step
    c.movePosition(QTextCursor::Start);
    m_editor->setTextCursor(c);
    int count = 0;
    while (m_editor->find(m_find->text(), flags(false))) {
        m_editor->textCursor().insertText(m_replace->text());
        ++count;
    }
    c.endEditBlock();
    m_status->setText(tr("%n replaced", nullptr, count));
}

bool FindBar::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Esc closes the bar before it can close the window.
        if (key->key() == Qt::Key_Escape) {
            event->accept();
            if (event->type() == QEvent::KeyPress) {
                hide();
                m_editor->setFocus();
            }
            return true;
        }
        if (event->type() == QEvent::KeyPress && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            if (watched == m_replace)
                replaceOne();
            else
                findNext(key->modifiers() & Qt::ShiftModifier);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---- TextView -------------------------------------------------------------------------------

TextView::TextView(QWidget* parent)
    : QWidget(parent), m_editor(new CodeEditor(this)), m_highlighter(new SyntaxHighlighter(m_editor->document())),
      m_findBar(new FindBar(m_editor, this)), m_syntax(new QComboBox(this)), m_wrap(new QToolButton(this)),
      m_info(new QLabel(this)), m_edit(new QToolButton(this))
{
    using TI = QIcon::ThemeIcon;
    for (TextSyntax s : {TextSyntax::Plain, TextSyntax::Xml, TextSyntax::Ini, TextSyntax::Csv, TextSyntax::Json,
                         TextSyntax::Lua, TextSyntax::Shader, TextSyntax::QuickBms})
        m_syntax->addItem(toQString(syntaxName(s)), static_cast<int>(s));
    m_syntax->setToolTip(tr("Colour coding"));
    connect(m_syntax, &QComboBox::activated, this,
            [this] { m_highlighter->setSyntax(static_cast<TextSyntax>(m_syntax->currentData().toInt())); });

    m_wrap->setText(tr("Wrap"));
    m_wrap->setCheckable(true);
    m_wrap->setAutoRaise(true);
    m_wrap->setToolTip(tr("Wrap long lines"));
    m_wrap->setChecked(QSettings().value("text/wrap", false).toBool());
    applyWrap(m_wrap->isChecked());
    connect(m_wrap, &QToolButton::toggled, this, [this](bool on) {
        QSettings().setValue("text/wrap", on);
        applyWrap(on);
    });

    auto* find = barButton(icons::theme(TI::EditFind, QStyle::SP_FileDialogContentsView), tr("Find"), this);
    find->setToolTip(tr("Find (Ctrl+F)"));
    connect(find, &QToolButton::clicked, this, [this] { openFind(); });

    m_info->setForegroundRole(QPalette::PlaceholderText);

    m_edit->setIcon(icons::theme(TI::DocumentProperties, QStyle::SP_FileDialogDetailedView));
    m_edit->setText(tr("Edit"));
    m_edit->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_edit->setToolTip(tr("Open this file in the text editor (saving changes needs Edit Mode)"));
    m_edit->hide();
    connect(m_edit, &QToolButton::clicked, this, &TextView::editRequested);

    auto* findShortcut = new QAction(this);
    findShortcut->setShortcut(QKeySequence::Find);
    findShortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QAction::triggered, this, [this] { openFind(); });
    addAction(findShortcut);
    auto* replaceShortcut = new QAction(this);
    replaceShortcut->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    replaceShortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(replaceShortcut, &QAction::triggered, this, [this] { openFind(true); });
    addAction(replaceShortcut);
    auto* nextShortcut = new QAction(this);
    nextShortcut->setShortcuts({QKeySequence(Qt::Key_F3)});
    nextShortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(nextShortcut, &QAction::triggered, this, [this] { m_findBar->findNext(false); });
    addAction(nextShortcut);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 4);
    bar->addWidget(m_info, 1);
    bar->addWidget(find);
    bar->addWidget(m_wrap);
    bar->addWidget(m_syntax);
    bar->addWidget(m_edit);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_findBar);
    setReadOnly(true);
}

void TextView::setText(const QString& text, TextSyntax syntax, const QString& info)
{
    // Large files: no colour coding (it would freeze the window).
    const bool tooBig = text.size() > kHighlightLimit;
    m_highlighter->setSyntax(TextSyntax::Plain); // don't highlight while the text goes in
    m_editor->setPlainText(text);
    m_syntax->setEnabled(!tooBig);
    setSyntax(tooBig ? TextSyntax::Plain : syntax);
    setInfo(tooBig ? info + tr("  ·  colour coding off (large file)") : info);
}

void TextView::clear()
{
    m_highlighter->setSyntax(TextSyntax::Plain);
    m_editor->clear();
    m_info->clear();
    m_findBar->hide();
}

QString TextView::text() const
{
    return m_editor->toPlainText();
}

void TextView::setReadOnly(bool readOnly)
{
    m_editor->setReadOnly(readOnly);
    // Read-only text still shows a caret so it can be selected with the keyboard.
    m_editor->setTextInteractionFlags(readOnly ? Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard
                                               : Qt::TextEditorInteraction);
}

void TextView::setEditButtonVisible(bool visible)
{
    m_edit->setVisible(visible);
}

void TextView::setInfo(const QString& info)
{
    m_info->setText(info);
}

void TextView::setSyntax(TextSyntax syntax)
{
    m_syntax->setCurrentIndex(m_syntax->findData(static_cast<int>(syntax)));
    m_highlighter->setSyntax(syntax);
}

TextSyntax TextView::syntax() const
{
    return m_highlighter->syntax();
}

void TextView::openFind(bool replace)
{
    m_findBar->open(replace);
}

void TextView::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) // light/dark switch: new colour set
        m_highlighter->refreshColours();
}

void TextView::applyWrap(bool wrap)
{
    m_editor->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

} // namespace viewer::app
