#pragma once

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QWidget>

#include <optional>

class QLabel;
class QLineEdit;

namespace viewer::app {

/// Classic offset | hex | ASCII dump. Paints only the visible lines, so it
/// stays fast for multi-hundred-megabyte buffers. Colours come from the
/// widget palette and therefore follow light/dark mode automatically.
///
/// A cursor and a selection: click or drag in either column, Shift extends,
/// arrows / Page Up / Page Down / Home / End move, Ctrl+A selects everything.
/// Ctrl+C copies the selection as hex; the context menu also copies it as a C
/// array or as text.
class HexView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit HexView(QWidget* parent = nullptr);

    void setData(QByteArray data);
    void clear();
    [[nodiscard]] const QByteArray& data() const noexcept { return m_data; }

    /// Highlights [offset, offset + length) and scrolls it into view (the
    /// Structure tab's selection; the user's own selection is separate).
    void highlight(qint64 offset, qint64 length);

    /// Moves the cursor to `offset` (selecting `length` bytes from there) and scrolls to it.
    void select(qint64 offset, qint64 length = 0);
    [[nodiscard]] qint64 cursor() const noexcept { return m_cursor; }
    /// The selected bytes: first offset and count (count 0: nothing selected).
    [[nodiscard]] qint64 selectionStart() const noexcept;
    [[nodiscard]] qint64 selectionLength() const noexcept;

    enum class CopyFormat { Hex, CArray, Text };
    void copySelection(CopyFormat format) const;

    /// Searches forward from after the cursor (wrapping) and selects the
    /// match. False if `needle` isn't there.
    bool findNext(const QByteArray& needle);

signals:
    /// The cursor or the selection changed.
    void cursorChanged();
    void goToRequested();
    void findRequested();
    void findNextRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void updateMetrics();
    void updateScrollBar();
    [[nodiscard]] qint64 lineCount() const;
    [[nodiscard]] int visibleLines() const;
    /// The byte under a viewport position (hex or ASCII column), or nullopt.
    [[nodiscard]] std::optional<qint64> byteAt(const QPoint& pos) const;
    /// Moves the cursor; with `extend` the selection grows from its anchor.
    void moveCursor(qint64 offset, bool extend);
    void ensureVisible(qint64 offset);

    QByteArray m_data;
    qint64 m_highlightStart = -1;
    qint64 m_highlightLength = 0;
    qint64 m_cursor = 0;
    qint64 m_anchor = -1; ///< Other end of the selection (-1: nothing selected).
    int m_lineHeight = 16;
    int m_charWidth = 8;
    int m_offsetDigits = 8;
};

/// The Hex tab: a strip with "Go to offset" and "Find", a read-out of the
/// cursor (offset, selection, and the value there as 8/16/32-bit integers and
/// a float), and the dump.
class HexPanel : public QWidget {
    Q_OBJECT
public:
    explicit HexPanel(QWidget* parent = nullptr);

    [[nodiscard]] HexView* view() const noexcept { return m_view; }

    /// Parses what the user typed in Find: hex bytes ("4B 41 50 32", "4b415032")
    /// or text in quotes ("KAP2"); anything else is searched as text.
    [[nodiscard]] static QByteArray parseNeedle(const QString& text);
    /// Parses an offset: decimal, or hex with 0x / h / $ or containing a-f.
    [[nodiscard]] static std::optional<qint64> parseOffset(const QString& text);

private:
    void updateStatus();
    void goTo();
    void find();

    HexView* m_view;
    QLineEdit* m_goTo;
    QLineEdit* m_find;
    QLabel* m_status;
};

} // namespace viewer::app
