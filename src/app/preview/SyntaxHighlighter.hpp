#pragma once

#include "viewer/core/Text.hpp"

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <array>

namespace viewer::app {

/// Colour-codes a text document for one TextSyntax. Colours follow the
/// palette (a light and a dark set), so call refreshColours() after a theme change.
class SyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit SyntaxHighlighter(QTextDocument* document);

    void setSyntax(TextSyntax syntax);
    [[nodiscard]] TextSyntax syntax() const noexcept { return m_syntax; }
    /// Re-reads the palette (light/dark) and re-highlights.
    void refreshColours();

protected:
    void highlightBlock(const QString& text) override;

private:
    /// Block states carried from one line to the next (multi-line comments etc.).
    enum State { Normal = -1, InComment = 1, InTag = 2, InCData = 3, InLongComment = 4, InQuote = 5 };

    void highlightXml(const QString& text);
    void highlightIni(const QString& text);
    void highlightCsv(const QString& text);
    void highlightCode(const QString& text);

    TextSyntax m_syntax = TextSyntax::Plain;
    QTextCharFormat m_keyword, m_type, m_string, m_number, m_comment, m_tag, m_attribute, m_value, m_entity,
        m_section, m_key, m_punctuation, m_preprocessor;
    std::array<QTextCharFormat, 6> m_columns;
};

} // namespace viewer::app
