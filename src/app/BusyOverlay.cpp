#include "BusyOverlay.hpp"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace viewer::app {

BusyOverlay::BusyOverlay(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
    parent->installEventFilter(this);
    m_delay.setSingleShot(true);
    connect(&m_delay, &QTimer::timeout, this, [this] {
        setGeometry(parentWidget()->rect());
        raise();
        show();
        m_spin.start();
    });
    m_spin.setInterval(16);
    connect(&m_spin, &QTimer::timeout, this, [this] {
        m_angle = (m_angle + 6) % 360;
        update(cardRect());
    });
}

void BusyOverlay::start(const QString& text, int delayMs)
{
    m_text = text;
    m_busy = true;
    if (isVisible())
        update();
    else
        m_delay.start(std::max(0, delayMs));
}

void BusyOverlay::setText(const QString& text)
{
    m_text = text;
    update();
}

void BusyOverlay::stop()
{
    m_busy = false;
    m_delay.stop();
    m_spin.stop();
    hide();
}

bool BusyOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

QRect BusyOverlay::cardRect() const
{
    const int textWidth = fontMetrics().horizontalAdvance(m_text);
    const int w = std::clamp(textWidth + 48, 220, std::max(220, width() - 32));
    const int h = 120;
    return {(width() - w) / 2, (height() - h) / 2, w, h};
}

void BusyOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();

    // Dim what's underneath, in the theme's own colour.
    QColor dim = pal.color(QPalette::Window);
    dim.setAlpha(150);
    p.fillRect(rect(), dim);

    // The card.
    const QRect card = cardRect();
    QColor border = pal.color(QPalette::WindowText);
    border.setAlpha(40);
    p.setPen(QPen(border, 1));
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

    // A ring with a turning arc.
    const int d = 34;
    const QRectF ring(card.center().x() - d / 2.0, card.top() + 20, d, d);
    QColor track = pal.color(QPalette::Highlight);
    track.setAlpha(50);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(ring);
    p.setPen(QPen(pal.color(QPalette::Highlight), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(ring, -m_angle * 16, 100 * 16);

    // The message under it.
    p.setPen(pal.color(QPalette::Text));
    const QRect textRect(card.left() + 16, static_cast<int>(ring.bottom()) + 10, card.width() - 32,
                         card.bottom() - static_cast<int>(ring.bottom()) - 18);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
               fontMetrics().elidedText(m_text, Qt::ElideMiddle, textRect.width()));
}

} // namespace viewer::app
