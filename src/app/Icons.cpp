#include "Icons.hpp"

#include <QApplication>
#include <QFileIconProvider>
#include <QHash>

namespace viewer::app::icons {

namespace {

/// Colour-coded file type icons shipped in resources/icons/types. Each uses a
/// mid-saturation accent so it reads on both light and dark backgrounds.
QIcon typeIcon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.insert(name, QIcon(QStringLiteral(":/icons/types/%1.svg").arg(name)));
    return *it;
}

} // namespace

QIcon theme(QIcon::ThemeIcon icon, QStyle::StandardPixmap fallback)
{
    QIcon result = QIcon::fromTheme(icon);
    if (result.isNull())
        result = QApplication::style()->standardIcon(fallback);
    return result;
}

QIcon folder()
{
    // The shell's own folder icon matches Explorer exactly.
    static const QIcon icon = QFileIconProvider().icon(QFileIconProvider::Folder);
    return icon;
}

QIcon archive()
{
    return typeIcon(QStringLiteral("archive"));
}

QIcon forCategory(FileCategory category)
{
    switch (category) {
    case FileCategory::Archive: return archive();
    case FileCategory::Texture: return typeIcon(QStringLiteral("texture"));
    case FileCategory::Model: return typeIcon(QStringLiteral("model"));
    case FileCategory::Animation: return typeIcon(QStringLiteral("animation"));
    case FileCategory::Audio: return typeIcon(QStringLiteral("audio"));
    case FileCategory::Video: return typeIcon(QStringLiteral("video"));
    case FileCategory::Text: return typeIcon(QStringLiteral("text"));
    case FileCategory::Data: return typeIcon(QStringLiteral("data"));
    case FileCategory::Shader: return typeIcon(QStringLiteral("shader"));
    case FileCategory::Level: return typeIcon(QStringLiteral("level"));
    case FileCategory::Font: return typeIcon(QStringLiteral("font"));
    case FileCategory::Executable: return typeIcon(QStringLiteral("executable"));
    case FileCategory::Unknown: break;
    }
    return typeIcon(QStringLiteral("file"));
}

QIcon appIcon()
{
    static const QIcon icon(QStringLiteral(":/icons/app.svg"));
    return icon;
}

} // namespace viewer::app::icons
