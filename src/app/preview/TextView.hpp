#pragma once

#include "viewer/core/Text.hpp"

#include <QPlainTextEdit>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace viewer::app {

class SyntaxHighlighter;

/// A plain-text editor with a line-number gutter and a highlighted current line.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parent = nullptr);

    [[nodiscard]] int gutterWidth() const;
    void paintGutter(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateGutter(const QRect& rect, int dy);
    void highlightCurrentLine();

    QWidget* m_gutter;
};

/// Find (and, when the text is editable, replace) bar shown under the text.
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(CodeEditor* editor, QWidget* parent = nullptr);

    /// Shows the bar with the selection (if any) as the search text.
    void open(bool replace);
    bool findNext(bool backwards = false);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void replaceOne();
    void replaceAll();
    [[nodiscard]] QTextDocument::FindFlags flags(bool backwards) const;

    CodeEditor* m_editor;
    QLineEdit* m_find;
    QLineEdit* m_replace;
    QCheckBox* m_matchCase;
    QCheckBox* m_wholeWords;
    QWidget* m_replaceRow;
    QLabel* m_status;
};

/// Text with colour coding, line numbers and find: the Preview tab for text
/// files (read-only, with an Edit button) and the body of TextEditorWindow.
class TextView : public QWidget {
    Q_OBJECT
public:
    explicit TextView(QWidget* parent = nullptr);

    /// Shows `text`. `info` describes it (encoding, line endings) in the bar.
    void setText(const QString& text, TextSyntax syntax, const QString& info);
    void clear();
    [[nodiscard]] QString text() const;
    [[nodiscard]] CodeEditor* editor() const noexcept { return m_editor; }

    void setReadOnly(bool readOnly);
    /// The bar's "Edit" button (previews only).
    void setEditButtonVisible(bool visible);
    void setInfo(const QString& info);
    void setSyntax(TextSyntax syntax);
    [[nodiscard]] TextSyntax syntax() const;
    void openFind(bool replace = false);

signals:
    void editRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void applyWrap(bool wrap);

    CodeEditor* m_editor;
    SyntaxHighlighter* m_highlighter;
    FindBar* m_findBar;
    QComboBox* m_syntax;
    QToolButton* m_wrap;
    QLabel* m_info;
    QToolButton* m_edit;
};

} // namespace viewer::app
