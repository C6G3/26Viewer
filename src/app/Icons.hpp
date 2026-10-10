#pragma once

#include "viewer/core/FileTypes.hpp"

#include <QIcon>
#include <QStyle>

namespace viewer::app::icons {

/// Freedesktop/Fluent theme icon with a QStyle fallback. On Windows 10/11
/// Qt draws theme icons with the Segoe Fluent/MDL2 icon font, so they match
/// the OS and recolour automatically in dark mode.
QIcon theme(QIcon::ThemeIcon icon, QStyle::StandardPixmap fallback);

QIcon folder();
QIcon archive();
QIcon forCategory(FileCategory category);
QIcon appIcon();

} // namespace viewer::app::icons
