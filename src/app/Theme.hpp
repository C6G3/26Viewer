#pragma once

#include <QObject>

class QActionGroup;
class QMenu;

namespace viewer::app {

/// Light / dark handling.
///
/// "System" follows the Windows app-mode setting *live*: Qt watches the OS
/// and emits QStyleHints::colorSchemeChanged, and the Windows 11 style plus
/// the dark title bar update automatically. "Light"/"Dark" force a scheme via
/// QStyleHints::setColorScheme (Qt 6.8+).
class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum class Mode { System, Light, Dark };
    Q_ENUM(Mode)

    explicit ThemeManager(QObject* parent = nullptr);

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    /// Applies a mode; `persist` = false for one-off overrides (command line).
    void setMode(Mode mode, bool persist = true);
    [[nodiscard]] bool isDark() const;

    /// Adds a checkable System/Light/Dark group to `menu`.
    void populateMenu(QMenu* menu);

signals:
    /// Emitted whenever the effective appearance changes (mode or OS switch).
    void appearanceChanged(bool dark);

private:
    void apply();

    Mode m_mode = Mode::System;
    QActionGroup* m_group = nullptr;
};

} // namespace viewer::app
