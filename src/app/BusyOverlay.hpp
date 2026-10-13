#pragma once

#include <QTimer>
#include <QWidget>

namespace viewer::app {

/// "Loading…" popup: a card with a spinning ring and a message in the middle
/// of its parent widget, over a dimmed copy of it. It covers the parent (and
/// keeps doing so when it resizes), so clicks don't reach what's loading.
/// Shown only when the work takes longer than a moment, so quick loads don't flash.
class BusyOverlay : public QWidget {
    Q_OBJECT
public:
    explicit BusyOverlay(QWidget* parent);

    /// Shows `text` after `delayMs` (unless stop() comes first).
    void start(const QString& text, int delayMs = 250);
    void setText(const QString& text);
    void stop();
    [[nodiscard]] bool isBusy() const noexcept { return m_busy; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] QRect cardRect() const;

    QString m_text;
    QTimer m_delay;
    QTimer m_spin;
    int m_angle = 0;
    bool m_busy = false;
};

} // namespace viewer::app
