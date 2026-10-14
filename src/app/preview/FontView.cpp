#include "FontView.hpp"

#include "../QtConvert.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace viewer::app {

namespace {

constexpr double kSampleSizes[] = {72, 48, 32, 20};
constexpr int kMargin = 16;
constexpr int kCell = 64;        ///< Character grid cell (square), plus the code under it.
constexpr int kCodeHeight = 16;

QString defaultSample()
{
    return QObject::tr("The quick brown fox jumps over the lazy dog 0123456789");
}

/// A closed contour as a path: lines to points on the outline, curves through control points.
void addContour(QPainterPath& path, const std::vector<Font::Point>& points)
{
    using Kind = Font::Point::Kind;
    const std::size_t n = points.size();
    if (n < 2)
        return;
    // Start at a point on the outline (a contour may begin with a control point).
    std::size_t start = 0;
    while (start < n && points[start].kind != Kind::On)
        ++start;
    if (start == n)
        start = 0;
    const auto at = [&](std::size_t i) { return QPointF(points[(start + i) % n].x, points[(start + i) % n].y); };
    path.moveTo(at(0));
    for (std::size_t i = 1; i <= n;) {
        const Kind kind = points[(start + i) % n].kind;
        if (kind == Kind::Cubic && i + 2 <= n) {
            path.cubicTo(at(i), at(i + 1), at(i + 2));
            i += 3;
        } else if (kind == Kind::Quadratic && i + 1 <= n) {
            path.quadTo(at(i), at(i + 1));
            i += 2;
        } else {
            path.lineTo(at(i));
            ++i;
        }
    }
    path.closeSubpath();
}

} // namespace

FontCanvas::FontCanvas(QWidget* parent) : QWidget(parent), m_sample(defaultSample())
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void FontCanvas::setTypeface(std::shared_ptr<const Font> font)
{
    m_font = std::move(font);
    m_outlines.clear();
    updateGeometry();
    update();
}

void FontCanvas::setSample(const QString& text)
{
    m_sample = text;
    update();
}

const QPainterPath& FontCanvas::outline(const Font::Glyph& glyph)
{
    auto [it, added] = m_outlines.try_emplace(glyph.code);
    if (added) {
        it->second.setFillRule(Qt::WindingFill); // outlines from TrueType: holes run the other way
        for (const auto& contour : glyph.contours)
            addContour(it->second, contour);
    }
    return it->second;
}

void FontCanvas::drawText(QPainter& painter, const QString& text, QPointF origin, double pixels)
{
    if (!m_font || m_font->lineHeight <= 0)
        return;
    const double scale = pixels / m_font->lineHeight;
    double pen = origin.x();
    char32_t previous = 0;
    for (const char32_t c : text.toUcs4()) {
        const Font::Glyph* glyph = m_font->find(c);
        if (previous)
            pen += m_font->kern(previous, c) * scale;
        previous = c;
        if (!glyph) { // not in this font: an empty box
            const double w = m_font->lineHeight * 0.4 * scale;
            if (c != U' ') {
                painter.save();
                painter.setPen(QPen(painter.brush().color(), 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(QRectF(pen + w * 0.1, origin.y() - pixels * 0.6, w * 0.8, pixels * 0.6));
                painter.restore();
            }
            pen += w;
            continue;
        }
        QTransform transform;
        transform.translate(pen + glyph->leftSpace * scale, origin.y() + m_font->baseline * scale);
        transform.scale(scale, -scale); // font y points up
        painter.drawPath(transform.map(outline(*glyph)));
        pen += glyph->advance() * scale;
        if (pen > width())
            break;
    }
}

int FontCanvas::layoutHeight(int width) const
{
    if (!m_font)
        return 0;
    int y = kMargin;
    for (const double size : kSampleSizes)
        y += static_cast<int>(size * 1.25);
    y += kMargin;
    const int columns = std::max(1, (width - 2 * kMargin) / kCell);
    const int rows = (static_cast<int>(m_font->glyphs.size()) + columns - 1) / columns;
    return y + rows * (kCell + kCodeHeight) + kMargin;
}

QSize FontCanvas::sizeHint() const
{
    return {600, layoutHeight(600)};
}

int FontCanvas::heightForWidth(int width) const
{
    return layoutHeight(width);
}

void FontCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (const int wanted = layoutHeight(width()); wanted != height())
        setMinimumHeight(wanted);
}

void FontCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    if (!m_font)
        return;
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor ink = palette().color(QPalette::Text);
    QColor faint = ink;
    faint.setAlpha(60);

    // Sample text, large to small.
    double y = kMargin;
    for (const double size : kSampleSizes) {
        y += size;
        painter.setPen(QPen(faint, 1));
        painter.drawLine(QPointF(kMargin, y - size * 0.2), QPointF(width() - kMargin, y - size * 0.2)); // baseline
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        drawText(painter, m_sample, QPointF(kMargin, y - size * 0.2), size);
        y += size * 0.25;
    }
    y += kMargin;

    // Every character, with its code.
    const int columns = std::max(1, (width() - 2 * kMargin) / kCell);
    QFont small = font();
    small.setPointSizeF(std::max(6.5, small.pointSizeF() * 0.8));
    painter.setFont(small);
    for (std::size_t i = 0; i < m_font->glyphs.size(); ++i) {
        const Font::Glyph& g = m_font->glyphs[i];
        const QRectF cell(kMargin + static_cast<double>(i % columns) * kCell,
                          y + static_cast<double>(i / columns) * (kCell + kCodeHeight), kCell, kCell + kCodeHeight);
        painter.setPen(QPen(faint, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
        const double pixels = kCell * 0.8;
        const double scale = pixels / m_font->lineHeight;
        const double left = cell.left() + (kCell - g.width * scale) / 2;
        QTransform transform;
        transform.translate(left, cell.top() + kCell * 0.9);
        transform.scale(scale, -scale);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(transform.map(outline(g)));
        QColor label = ink;
        label.setAlpha(150);
        painter.setPen(label);
        painter.drawText(QRectF(cell.left(), cell.top() + kCell, kCell, kCodeHeight), Qt::AlignCenter,
                         QStringLiteral("U+%1").arg(static_cast<uint>(g.code), 4, 16, QLatin1Char('0')).toUpper());
    }
}

// ---- FontView ------------------------------------------------------------------------------

FontView::FontView(QWidget* parent)
    : QWidget(parent), m_info(new QLabel(this)), m_sample(new QLineEdit(this)), m_scroll(new QScrollArea(this)),
      m_canvas(new FontCanvas)
{
    m_info->setForegroundRole(QPalette::PlaceholderText);
    m_sample->setPlaceholderText(tr("Type your own sample text"));
    m_sample->setClearButtonEnabled(true);
    m_sample->setText(QSettings().value("preview/font/sample", defaultSample()).toString());
    m_canvas->setSample(m_sample->text().isEmpty() ? defaultSample() : m_sample->text());
    connect(m_sample, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_canvas->setSample(text.isEmpty() ? defaultSample() : text);
        QSettings().setValue("preview/font/sample", text);
    });
    m_scroll->setWidget(m_canvas);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(8, 6, 8, 6);
    top->addWidget(m_sample, 1);
    top->addWidget(m_info);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(m_scroll, 1);
}

void FontView::setTypeface(std::shared_ptr<const Font> font)
{
    m_info->setText(font ? tr("%1 · %n character(s) · %2 kerning pair(s)", nullptr,
                              static_cast<int>(font->glyphs.size()))
                               .arg(toQString(font->name))
                               .arg(font->kerning.size())
                         : QString());
    m_canvas->setTypeface(std::move(font));
}

void FontView::clear()
{
    setTypeface(nullptr);
}

} // namespace viewer::app
