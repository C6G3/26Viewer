#pragma once

#include <QAbstractScrollArea>
#include <QByteArray>

namespace viewer::app {

/// Classic offset | hex | ASCII dump. Paints only the visible lines, so it
/// stays fast for multi-hundred-megabyte buffers. Colours come from the
/// widget palette and therefore follow light/dark mode automatically.
class HexView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit HexView(QWidget* parent = nullptr);

    void setData(QByteArray data);
    void clear();
    [[nodiscard]] const QByteArray& data() const noexcept { return m_data; }

    /// Highlights [offset, offset + length) and scrolls it into view.
    void highlight(qint64 offset, qint64 length);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateMetrics();
    void updateScrollBar();
    [[nodiscard]] qint64 lineCount() const;
    [[nodiscard]] int visibleLines() const;

    QByteArray m_data;
    qint64 m_highlightStart = -1;
    qint64 m_highlightLength = 0;
    int m_lineHeight = 16;
    int m_charWidth = 8;
    int m_offsetDigits = 8;
};

} // namespace viewer::app
