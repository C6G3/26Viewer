#include "ImageView.hpp"

#include "../Icons.hpp"
#include "../QtConvert.hpp"

#include "viewer/core/Image.hpp"

#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
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

void ImageCanvas::setImage(const QImage& image, bool keepView)
{
    const QRectF before = m_item->boundingRect();
    const QPointF centre = mapToScene(viewport()->rect().center());
    const double shown = before.width() * zoom();
    m_item->setPixmap(QPixmap::fromImage(image));
    const QRectF after = m_item->boundingRect();
    scene()->setSceneRect(after);
    if (!keepView || before.isEmpty() || after.isEmpty()) {
        fit();
        return;
    }
    // The same on-screen size, and the same spot of the picture in the middle.
    const double z = shown / after.width();
    setTransform(QTransform::fromScale(z, z));
    centerOn(centre.x() * after.width() / before.width(), centre.y() * after.height() / before.height());
    emit zoomChanged(zoom());
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

namespace {

enum Channel : unsigned { Red = 1, Green = 2, Blue = 4, Alpha = 8, All = 15 };

QString layerName(std::uint32_t layer, std::uint32_t layers)
{
    static const char* const kFaces[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    if (layers == 6)
        return ImageView::tr("Face %1").arg(QLatin1String(kFaces[layer]));
    return ImageView::tr("Layer %1").arg(layer);
}

} // namespace

ImageView::ImageView(QWidget* parent)
    : QWidget(parent), m_canvas(new ImageCanvas(this)), m_info(new QLabel(this)), m_zoomLabel(new QLabel(this)),
      m_mip(new QComboBox(this)), m_layer(new QComboBox(this))
{
    auto* fitButton = new QToolButton(this);
    fitButton->setIcon(icons::theme(QIcon::ThemeIcon::ZoomFitBest, QStyle::SP_TitleBarMaxButton));
    fitButton->setToolTip(tr("Fit to window"));
    fitButton->setAutoRaise(true);
    auto* oneToOne = new QToolButton(this);
    oneToOne->setText(QStringLiteral("1:1"));
    oneToOne->setToolTip(tr("Actual size (100%)"));
    oneToOne->setAutoRaise(true);
    auto* save = new QToolButton(this);
    save->setIcon(icons::theme(QIcon::ThemeIcon::DocumentSaveAs, QStyle::SP_DialogSaveButton));
    save->setText(tr("PNG…"));
    save->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    save->setToolTip(tr("Save what is shown (this mip level, these channels) as a PNG picture"));
    save->setAutoRaise(true);

    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mip->setToolTip(tr("Mip level: the smaller copies the game draws at a distance"));
    m_mip->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_layer->setToolTip(tr("Cube map face or texture layer"));
    m_layer->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_mip->hide();
    m_layer->hide();

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 4);
    bar->setSpacing(2);
    bar->addWidget(m_info, 1);
    bar->addWidget(m_layer);
    bar->addWidget(m_mip);
    bar->addSpacing(8);
    static const char* const kNames[] = {"R", "G", "B", "A"};
    const QString tips[] = {tr("Red channel"), tr("Green channel"), tr("Blue channel"),
                            tr("Alpha channel (transparency)")};
    for (std::size_t i = 0; i < m_channels.size(); ++i) {
        auto* b = new QToolButton(this);
        b->setText(QLatin1String(kNames[i]));
        b->setCheckable(true);
        b->setChecked(true);
        b->setAutoRaise(true);
        b->setToolTip(tips[i] + tr("\nClick: show or hide. Ctrl+click: only this one (again: all)."));
        connect(b, &QToolButton::clicked, this, [this, i](bool checked) {
            if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
                // Before the click this channel was on; was it the only one?
                const bool wasAlone = !checked && (channelMask() & ~(1u << i)) == 0;
                for (std::size_t j = 0; j < m_channels.size(); ++j)
                    m_channels[j]->setChecked(wasAlone || j == i);
            }
            refresh(true);
        });
        // The style fills a checked button with the accent colour; keep its letter readable on it.
        const auto letterColour = [b](bool on) {
            QPalette pal = b->parentWidget()->palette();
            if (on)
                pal.setColor(QPalette::ButtonText, pal.color(QPalette::HighlightedText));
            b->setPalette(pal);
        };
        connect(b, &QToolButton::toggled, b, letterColour);
        letterColour(true);
        m_channels[i] = b;
        bar->addWidget(b);
    }
    bar->addSpacing(8);
    bar->addWidget(m_zoomLabel);
    bar->addWidget(fitButton);
    bar->addWidget(oneToOne);
    bar->addWidget(save);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_canvas, 1);

    connect(fitButton, &QToolButton::clicked, m_canvas, &ImageCanvas::fit);
    connect(oneToOne, &QToolButton::clicked, m_canvas, &ImageCanvas::actualSize);
    connect(save, &QToolButton::clicked, this, &ImageView::savePng);
    connect(m_mip, &QComboBox::activated, this, &ImageView::showLevel);
    connect(m_layer, &QComboBox::activated, this, &ImageView::showLevel);
    connect(m_canvas, &ImageCanvas::zoomChanged, this,
            [this](double z) { m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });
}

void ImageView::setImage(const QImage& image, const QString& info, const QString& name)
{
    m_top = image;
    m_image = image;
    m_name = name;
    m_info->setText(info);
    setSurfaces(nullptr);
    refresh(false);
}

void ImageView::setSurfaces(std::shared_ptr<const image::Surfaces> surfaces)
{
    m_surfaces = std::move(surfaces);
    m_mip->clear();
    m_layer->clear();
    m_mip->setVisible(m_surfaces && m_surfaces->mipCount > 1);
    m_layer->setVisible(m_surfaces && m_surfaces->layers > 1);
    if (!m_surfaces)
        return;
    for (std::uint32_t m = 0; m < m_surfaces->mipCount; ++m)
        m_mip->addItem(tr("Mip %1 · %2 × %3")
                           .arg(m)
                           .arg(std::max(1u, m_surfaces->width >> m))
                           .arg(std::max(1u, m_surfaces->height >> m)));
    for (std::uint32_t l = 0; l < m_surfaces->layers; ++l)
        m_layer->addItem(layerName(l, m_surfaces->layers));
}

void ImageView::clear()
{
    setImage({}, {});
}

void ImageView::showLevel()
{
    const int mip = std::max(0, m_mip->currentIndex());
    const int layer = std::max(0, m_layer->currentIndex());
    if (!m_surfaces || (mip == 0 && layer == 0)) {
        m_image = m_top;
    } else if (auto img = image::decodeLevel(*m_surfaces, static_cast<std::uint32_t>(layer),
                                             static_cast<std::uint32_t>(mip))) {
        m_image = QImage(img->rgba.data(), static_cast<int>(img->width), static_cast<int>(img->height),
                         static_cast<qsizetype>(img->width) * 4, QImage::Format_RGBA8888)
                      .copy(); // detach from the decoder's buffer
    } else {
        m_image = {};
        m_info->setText(toQString(img.error().message));
    }
    refresh(true);
}

unsigned ImageView::channelMask() const
{
    unsigned mask = 0;
    for (std::size_t i = 0; i < m_channels.size(); ++i)
        if (m_channels[i]->isChecked())
            mask |= 1u << i;
    return mask;
}

QImage ImageView::channels(const QImage& image, unsigned mask)
{
    if (image.isNull() || mask == All)
        return image;
    QImage out = image.convertToFormat(QImage::Format_RGBA8888);
    const unsigned colour = mask & (Red | Green | Blue);
    // One channel on its own (or alpha alone) is shown as a grey picture of its values.
    int grey = -1;
    if (colour == Red || colour == Green || colour == Blue)
        grey = colour == Red ? 0 : colour == Green ? 1 : 2;
    else if (colour == 0 && (mask & Alpha))
        grey = 3;
    const bool keepAlpha = (mask & Alpha) && grey != 3;
    for (int y = 0; y < out.height(); ++y) {
        std::uint8_t* p = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x, p += 4) {
            if (grey >= 0) {
                p[0] = p[1] = p[2] = p[grey];
            } else {
                for (unsigned c = 0; c < 3; ++c)
                    if (!(colour & (1u << c)))
                        p[c] = 0;
            }
            if (!keepAlpha)
                p[3] = 255;
        }
    }
    return out;
}

void ImageView::refresh(bool keepView)
{
    m_canvas->setImage(channels(m_image, channelMask()), keepView);
}

void ImageView::savePng()
{
    if (m_image.isNull())
        return;
    // "flag_mip2_RG.png": the file name says which level and channels it holds.
    QString base = m_name.isEmpty() ? QStringLiteral("image") : QFileInfo(m_name).completeBaseName();
    if (m_layer->isVisible() && m_layer->currentIndex() > 0)
        base += QStringLiteral("_layer%1").arg(m_layer->currentIndex());
    if (m_mip->isVisible() && m_mip->currentIndex() > 0)
        base += QStringLiteral("_mip%1").arg(m_mip->currentIndex());
    if (const unsigned mask = channelMask(); mask != All) {
        base += QLatin1Char('_');
        for (std::size_t i = 0; i < m_channels.size(); ++i)
            if (mask & (1u << i))
                base += m_channels[i]->text();
    }
    QSettings settings;
    const QString lastDir = settings.value("paths/lastExtractDir").toString();
    const QString file = QFileDialog::getSaveFileName(this, tr("Save as PNG"),
                                                      QDir(lastDir).filePath(base + QStringLiteral(".png")),
                                                      tr("PNG picture (*.png)"));
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastExtractDir", QFileInfo(file).absolutePath());
    if (!channels(m_image, channelMask()).save(file, "PNG"))
        QMessageBox::warning(this, tr("Save as PNG"), tr("Could not write %1.").arg(QDir::toNativeSeparators(file)));
}

} // namespace viewer::app
