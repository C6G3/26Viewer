#include "ImageView.hpp"

#include "../Icons.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QGraphicsPixmapItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace viewer::app {

// ---- ImageCanvas ---------------------------------------------------------------------

ImageCanvas::ImageCanvas(QWidget* parent) : QGraphicsView(parent)
{
    setScene(new QGraphicsScene(this));
    m_item = scene()->addPixmap({});
    m_item->setTransformationMode(Qt::FastTransformation); // crisp pixels when zoomed in
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setRenderHint(QPainter::SmoothPixmapTransform, false);
    setFrameShape(QFrame::NoFrame);
    rebuildChecker();
}

void ImageCanvas::setImage(const QImage& image)
{
    m_item->setPixmap(QPixmap::fromImage(image));
    scene()->setSceneRect(m_item->boundingRect());
    m_fitMode = true;
    fit();
}

double ImageCanvas::zoom() const
{
    return transform().m11();
}

void ImageCanvas::fit()
{
    m_fitMode = true;
    resetTransform();
    const QRectF bounds = m_item->boundingRect();
    if (bounds.isEmpty())
        return;
    // Fit large images, but never upscale small ones past 100% automatically.
    if (bounds.width() > viewport()->width() || bounds.height() > viewport()->height())
        fitInView(bounds, Qt::KeepAspectRatio);
    centerOn(bounds.center());
    emit zoomChanged(zoom());
}

void ImageCanvas::actualSize()
{
    m_fitMode = false;
    resetTransform();
    centerOn(m_item->boundingRect().center());
    emit zoomChanged(zoom());
}

void ImageCanvas::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0)
        return;
    const double factor = std::pow(1.25, steps);
    const double next = zoom() * factor;
    if (next < 0.02 || next > 64)
        return;
    m_fitMode = false;
    scale(factor, factor);
    emit zoomChanged(zoom());
}

void ImageCanvas::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (m_fitMode)
        fit();
}

void ImageCanvas::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        rebuildChecker();
    QGraphicsView::changeEvent(event);
}

void ImageCanvas::rebuildChecker()
{
    // Two shades derived from the current palette, so dark mode gets a dark checkerboard.
    const QColor base = palette().color(QPalette::Base);
    const bool dark = base.lightnessF() < 0.5;
    const QColor a = dark ? base.lighter(130) : base.darker(108);
    const QColor b = dark ? base.lighter(160) : base.darker(118);
    m_checker = QPixmap(16, 16);
    m_checker.fill(a);
    QPainter p(&m_checker);
    p.fillRect(0, 0, 8, 8, b);
    p.fillRect(8, 8, 8, 8, b);
    viewport()->update();
}

void ImageCanvas::drawBackground(QPainter* painter, const QRectF& rect)
{
    painter->fillRect(rect, palette().color(QPalette::Window));
    const QRectF image = m_item->boundingRect();
    if (image.isEmpty())
        return;
    // Draw the checker in device pixels so it stays a constant size at any zoom.
    painter->save();
    painter->setWorldMatrixEnabled(false);
    const QRect deviceRect = mapFromScene(image).boundingRect();
    painter->fillRect(deviceRect, QBrush(m_checker));
    painter->restore();
}

// ---- ImageView -----------------------------------------------------------------------

ImageView::ImageView(QWidget* parent)
    : QWidget(parent), m_canvas(new ImageCanvas(this)), m_info(new QLabel(this)), m_zoomLabel(new QLabel(this)),
      m_alpha(new QCheckBox(tr("Alpha"), this))
{
    auto* fitButton = new QToolButton(this);
    fitButton->setIcon(icons::theme(QIcon::ThemeIcon::ZoomFitBest, QStyle::SP_TitleBarMaxButton));
    fitButton->setToolTip(tr("Fit to window"));
    fitButton->setAutoRaise(true);
    auto* oneToOne = new QToolButton(this);
    oneToOne->setText(QStringLiteral("1:1"));
    oneToOne->setToolTip(tr("Actual size (100%)"));
    oneToOne->setAutoRaise(true);

    m_alpha->setChecked(true);
    m_alpha->setToolTip(tr("Show the alpha channel as transparency"));
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 4);
    bar->addWidget(m_info, 1);
    bar->addWidget(m_alpha);
    bar->addSpacing(8);
    bar->addWidget(m_zoomLabel);
    bar->addWidget(fitButton);
    bar->addWidget(oneToOne);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_canvas, 1);

    connect(fitButton, &QToolButton::clicked, m_canvas, &ImageCanvas::fit);
    connect(oneToOne, &QToolButton::clicked, m_canvas, &ImageCanvas::actualSize);
    connect(m_alpha, &QCheckBox::toggled, this, &ImageView::refresh);
    connect(m_canvas, &ImageCanvas::zoomChanged, this,
            [this](double z) { m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });
}

void ImageView::setImage(const QImage& image, const QString& info)
{
    m_image = image;
    m_info->setText(info);
    refresh();
}

void ImageView::clear()
{
    m_image = {};
    m_info->clear();
    m_canvas->setImage({});
}

void ImageView::refresh()
{
    if (m_alpha->isChecked() || !m_image.hasAlphaChannel())
        m_canvas->setImage(m_image);
    else
        m_canvas->setImage(m_image.convertToFormat(QImage::Format_RGB32));
}

} // namespace viewer::app
