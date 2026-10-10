#pragma once

#include <QGraphicsView>
#include <QImage>
#include <QWidget>

class QCheckBox;
class QGraphicsPixmapItem;
class QLabel;

namespace viewer::app {

/// Zoomable image canvas: mouse wheel zooms around the cursor, drag pans,
/// transparent pixels sit on a palette-aware checkerboard.
class ImageCanvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget* parent = nullptr);

    void setImage(const QImage& image);
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

/// Image preview with an info bar (size/format) and zoom / alpha controls.
class ImageView : public QWidget {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);

    void setImage(const QImage& image, const QString& info);
    void clear();

private:
    void refresh();

    ImageCanvas* m_canvas;
    QLabel* m_info;
    QLabel* m_zoomLabel;
    QCheckBox* m_alpha;
    QImage m_image;
};

} // namespace viewer::app
