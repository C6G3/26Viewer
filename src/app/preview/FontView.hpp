#pragma once

#include "viewer/core/Font.hpp"

#include <QPainterPath>
#include <QWidget>

#include <map>
#include <memory>

class QLabel;
class QLineEdit;
class QScrollArea;

namespace viewer::app {

/// Draws a Font: sample text at a few sizes, then every character in a grid.
class FontCanvas : public QWidget {
    Q_OBJECT
public:
    explicit FontCanvas(QWidget* parent = nullptr);
    void setTypeface(std::shared_ptr<const Font> font);
    void setSample(const QString& text);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    /// The glyph's outline in font units (y up), built once.
    const QPainterPath& outline(const Font::Glyph& glyph);
    /// Draws `text` with its left end of the baseline at `origin`, `pixels` high per line.
    void drawText(QPainter& painter, const QString& text, QPointF origin, double pixels);
    /// Layout height for `width` (samples, then the character grid).
    [[nodiscard]] int layoutHeight(int width) const;

    std::shared_ptr<const Font> m_font;
    std::map<char32_t, QPainterPath> m_outlines;
    QString m_sample;
};

/// Font preview: a box for your own sample text over the FontCanvas.
class FontView : public QWidget {
    Q_OBJECT
public:
    explicit FontView(QWidget* parent = nullptr);
    void setTypeface(std::shared_ptr<const Font> font);
    void clear();

private:
    QLabel* m_info;
    QLineEdit* m_sample;
    QScrollArea* m_scroll;
    FontCanvas* m_canvas;
};

} // namespace viewer::app
