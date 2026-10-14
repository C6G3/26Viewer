#pragma once

#include <QGraphicsView>
#include <QImage>
#include <QWidget>

#include <array>
#include <memory>

class QComboBox;
class QGraphicsPixmapItem;
class QLabel;
class QToolButton;

namespace viewer::image {
struct Surfaces;
}

namespace viewer::app {

/// Zoomable image canvas: mouse wheel zooms around the cursor, drag pans,
/// transparent pixels sit on a palette-aware checkerboard.
class ImageCanvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget* parent = nullptr);

    /// Shows `image`, fitted to the window. With `keepView` the zoom and the
    /// centre stay as they were (a smaller mip level is drawn at the same size).
    void setImage(const QImage& image, bool keepView = false);
    void fit();
    void actualSize();
    [[nodiscard]] double zoom() const;

signals:
    void zoomChanged(double factor);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void rebuildChecker();

    QGraphicsPixmapItem* m_item = nullptr;
    QPixmap m_checker;
    bool m_fitMode = true;
};

/// Image preview with an info bar (size/format), R/G/B/A channel toggles
/// (one channel alone is shown as grey), mip level and face pickers for
/// textures, zoom controls and "Save PNG" (exactly what is shown).
class ImageView : public QWidget {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);

    /// `name` (the file's) suggests the PNG file name.
    void setImage(const QImage& image, const QString& info, const QString& name = {});
    /// Offers the texture's stored mip levels and layers (call after
    /// setImage; each is decoded when picked).
    void setSurfaces(std::shared_ptr<const image::Surfaces> surfaces);
    void clear();

    /// `image` with only the channels in `mask` (bit 0 R, 1 G, 2 B, 3 A):
    /// a colour channel alone, or alpha alone, becomes grey; without alpha
    /// the result is opaque.
    [[nodiscard]] static QImage channels(const QImage& image, unsigned mask);

private:
    void showLevel();
    void refresh(bool keepView);
    [[nodiscard]] unsigned channelMask() const;
    void savePng();

    ImageCanvas* m_canvas;
    QLabel* m_info;
    QLabel* m_zoomLabel;
    std::array<QToolButton*, 4> m_channels{};
    QComboBox* m_mip;
    QComboBox* m_layer;
    QImage m_top;   ///< Level 0 as loaded.
    QImage m_image; ///< The level shown.
    std::shared_ptr<const image::Surfaces> m_surfaces;
    QString m_name;
};

} // namespace viewer::app
