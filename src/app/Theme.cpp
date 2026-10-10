#include "Theme.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace viewer::app {

namespace {

constexpr auto kSettingsKey = "appearance/theme";

} // namespace

ThemeManager::ThemeManager(QObject* parent) : QObject(parent)
{
    const QString saved = QSettings().value(kSettingsKey, "system").toString();
    m_mode = saved == "light" ? Mode::Light : saved == "dark" ? Mode::Dark : Mode::System;

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this] { emit appearanceChanged(isDark()); });
    apply();
}

bool ThemeManager::isDark() const
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void ThemeManager::setMode(Mode mode, bool persist)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    if (persist)
        QSettings().setValue(kSettingsKey, mode == Mode::Light ? "light" : mode == Mode::Dark ? "dark" : "system");
    if (m_group)
        for (QAction* action : m_group->actions())
            action->setChecked(m_group->actions().indexOf(action) == static_cast<int>(mode));
    apply();
}

void ThemeManager::apply()
{
    auto* hints = QGuiApplication::styleHints();
    switch (m_mode) {
    case Mode::System: hints->unsetColorScheme(); break;
    case Mode::Light: hints->setColorScheme(Qt::ColorScheme::Light); break;
    case Mode::Dark: hints->setColorScheme(Qt::ColorScheme::Dark); break;
    }

    // The legacy "windowsvista" style (Windows 10) has no dark palette; Fusion
    // does. Windows 11 uses the "windows11" style which handles both.
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        const QString current = app->style()->name().toLower();
        if (isDark() && current == "windowsvista")
            app->setStyle(QStyleFactory::create("Fusion"));
    }
    emit appearanceChanged(isDark());
}

void ThemeManager::populateMenu(QMenu* menu)
{
    m_group = new QActionGroup(menu);
    const std::pair<Mode, QString> entries[] = {
        {Mode::System, tr("Use &System Setting")},
        {Mode::Light, tr("&Light")},
        {Mode::Dark, tr("&Dark")},
    };
    for (const auto& [mode, label] : entries) {
        QAction* action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(mode == m_mode);
        m_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setMode(mode); });
        if (mode == Mode::System)
            menu->addSeparator();
    }
}

} // namespace viewer::app
