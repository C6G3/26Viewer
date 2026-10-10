#include "HexView.hpp"

#include <QEvent>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

namespace viewer::app {

namespace {

constexpr int kBytesPerLine = 16;
constexpr int kMargin = 8;

QFont monospaceFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), font.family()});
    font.setStyleHint(QFont::Monospace);
    return font;
}

QColor blend(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

} // namespace

HexView::HexView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFont(monospaceFont());
    viewport()->setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Base);
    updateMetrics();
}

void HexView::setData(QByteArray data)
{
    m_data = std::move(data);
    m_highlightStart = -1;
    m_offsetDigits = m_data.size() > 0xFFFFFFFFll ? 10 : 8;
    verticalScrollBar()->setValue(0);
    updateScrollBar();
    viewport()->update();
}

void HexView::clear()
{
    setData({});
}

void HexView::highlight(qint64 offset, qint64 length)
{
    m_highlightStart = offset;
    m_highlightLength = length;
    const qint64 line = offset / kBytesPerLine;
    auto* bar = verticalScrollBar();
    if (line < bar->value() || line >= bar->value() + visibleLines())
        bar->setValue(static_cast<int>(std::max<qint64>(0, line - 2)));
    viewport()->update();
}

qint64 HexView::lineCount() const
{
    return (m_data.size() + kBytesPerLine - 1) / kBytesPerLine;
}

int HexView::visibleLines() const
{
    return std::max(1, viewport()->height() / m_lineHeight);
}

void HexView::updateMetrics()
{
    const QFontMetrics fm(font());
    m_lineHeight = fm.height() + 2;
    m_charWidth = fm.horizontalAdvance(QLatin1Char('0'));
    updateScrollBar();
}

void HexView::updateScrollBar()
{
    // Scroll range is in lines; QScrollBar's int range covers ~34 GB at 16 B/line.
    const qint64 maxLine = std::max<qint64>(0, lineCount() - visibleLines());
    verticalScrollBar()->setRange(0, static_cast<int>(std::min<qint64>(maxLine, INT_MAX)));
    verticalScrollBar()->setPageStep(visibleLines());
    const int width = kMargin * 2 + (m_offsetDigits + 2 + kBytesPerLine * 3 + 2 + kBytesPerLine) * m_charWidth;
    horizontalScrollBar()->setRange(0, std::max(0, width - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
}

void HexView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBar();
}

void HexView::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange)
        updateMetrics();
    QAbstractScrollArea::changeEvent(event);
}

void HexView::mousePressEvent(QMouseEvent* event)
{
    // Clicking clears a structure highlight.
    if (m_highlightStart >= 0) {
        m_highlightStart = -1;
        viewport()->update();
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void HexView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::Text);
    const QColor dim = blend(text, pal.color(QPalette::Base), 0.55);
    const QColor offsetColor = pal.color(QPalette::Link);
    const QColor highlightBg = blend(pal.color(QPalette::Highlight), pal.color(QPalette::Base), 0.55);

    p.setFont(font());
    const int ascent = QFontMetrics(font()).ascent() + 1;
    const int x0 = kMargin - horizontalScrollBar()->value();
    const int hexX = x0 + (m_offsetDigits + 2) * m_charWidth;
    const int asciiX = hexX + (kBytesPerLine * 3 + 2) * m_charWidth;

    if (m_data.isEmpty()) {
        p.setPen(dim);
        p.drawText(viewport()->rect(), Qt::AlignCenter, tr("No data"));
        return;
    }

    const qint64 first = verticalScrollBar()->value();
    const qint64 last = std::min(lineCount(), first + visibleLines() + 1);
    const auto* bytes = reinterpret_cast<const unsigned char*>(m_data.constData());
    static constexpr char kHex[] = "0123456789ABCDEF";

    for (qint64 line = first; line < last; ++line) {
        const int y = static_cast<int>(line - first) * m_lineHeight;
        const qint64 base = line * kBytesPerLine;

        p.setPen(offsetColor);
        p.drawText(x0, y + ascent, QStringLiteral("%1").arg(base, m_offsetDigits, 16, QLatin1Char('0')).toUpper());

        for (int i = 0; i < kBytesPerLine && base + i < m_data.size(); ++i) {
            const qint64 pos = base + i;
            const unsigned char b = bytes[pos];
            const int hx = hexX + (i * 3 + (i >= 8 ? 1 : 0)) * m_charWidth;
            const int ax = asciiX + i * m_charWidth;

            if (m_highlightStart >= 0 && pos >= m_highlightStart && pos < m_highlightStart + m_highlightLength) {
                p.fillRect(hx - m_charWidth / 2, y, m_charWidth * 3, m_lineHeight, highlightBg);
                p.fillRect(ax, y, m_charWidth, m_lineHeight, highlightBg);
            }

            p.setPen(b == 0 ? dim : text);
            const QChar hexPair[2] = {QLatin1Char(kHex[b >> 4]), QLatin1Char(kHex[b & 0xF])};
            p.drawText(hx, y + ascent, QString(hexPair, 2));
            p.setPen(b >= 0x20 && b < 0x7F ? text : dim);
            p.drawText(ax, y + ascent, QString(QLatin1Char(b >= 0x20 && b < 0x7F ? char(b) : '.')));
        }
    }
}

} // namespace viewer::app
