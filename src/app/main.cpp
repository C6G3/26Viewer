// 26Viewer — Qt desktop front-end.

#include "MainWindow.hpp"
#include "Theme.hpp"
#include "preview/PreviewPane.hpp"

#include "games/GameModules.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QTimer>

namespace {

struct Options {
    QString path;
    QString select;
    QString screenshot;
    QString theme;
    int tab = -1;
    bool editMode = false;
};

Options parseArguments(const QApplication& app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Modular game archive viewer"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Game folder or archive to open."));
    const QCommandLineOption select(QStringLiteral("select"), QStringLiteral("Virtual path to select after loading."),
                                    QStringLiteral("path"));
    const QCommandLineOption tab(QStringLiteral("tab"),
                                 QStringLiteral("Preview tab to show: preview, structure, hex or properties."),
                                 QStringLiteral("name"));
    const QCommandLineOption theme(QStringLiteral("theme"),
                                   QStringLiteral("Theme for this run only: system, light or dark."),
                                   QStringLiteral("mode"));
    const QCommandLineOption screenshot(
        QStringLiteral("screenshot"),
        QStringLiteral("Render off-screen, save a PNG of the window and exit (docs / UI checks)."),
        QStringLiteral("file"));
    const QCommandLineOption editMode(QStringLiteral("edit-mode"),
                                      QStringLiteral("Start with Edit Mode on, without the confirmation (automation)."));
    parser.addOptions({select, tab, theme, screenshot, editMode});
    parser.process(app);

    Options o;
    o.path = parser.positionalArguments().value(0);
    o.select = parser.value(select);
    o.screenshot = parser.value(screenshot);
    o.theme = parser.value(theme).toLower();
    o.editMode = parser.isSet(editMode);
    const QStringList tabs{"preview", "structure", "hex", "properties"};
    o.tab = tabs.indexOf(parser.value(tab).toLower());
    return o;
}

/// --screenshot: once loaded (and the selection previewed), grab and quit.
void scheduleScreenshot(viewer::app::MainWindow& window, const Options& o)
{
    auto capture = [&window, file = o.screenshot] {
        QTimer::singleShot(400, &window, [&window, file] {
            window.grab().save(file);
            QApplication::quit();
        });
    };
    QObject::connect(&window, &viewer::app::MainWindow::sessionChanged, &window, [&window, o, capture] {
        // A folder (or nothing) is shown at once; a file once its preview has loaded.
        if (o.select.isEmpty() || !window.selectPath(o.select) || !window.previewShowing()) {
            capture();
            return;
        }
        auto* pane = window.previewPane();
        QObject::connect(pane, &viewer::app::PreviewPane::previewReady, &window, [pane, o, capture] {
            if (o.tab >= 0)
                pane->setCurrentTab(o.tab);
            capture();
        }, Qt::SingleShotConnection);
    });
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("26Viewer"));
    QApplication::setApplicationName(QStringLiteral("26Viewer"));
    QApplication::setApplicationDisplayName(QStringLiteral("26Viewer"));
    QApplication::setApplicationVersion(QStringLiteral(VIEWER_VERSION));
    // Settings live in a readable INI file (%APPDATA%/26Viewer/26Viewer.ini)
    // instead of the registry.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg"))); // all windows and dialogs

    const Options options = parseArguments(app);

    viewer::GameRegistry games;
    viewer::registerBuiltinGames(games);

    viewer::app::ThemeManager theme;
    if (options.theme == "light")
        theme.setMode(viewer::app::ThemeManager::Mode::Light, false);
    else if (options.theme == "dark")
        theme.setMode(viewer::app::ThemeManager::Mode::Dark, false);
    else if (options.theme == "system")
        theme.setMode(viewer::app::ThemeManager::Mode::System, false);

    viewer::app::MainWindow window(games, theme);
    if (options.editMode)
        window.setEditMode(true, /*confirm=*/false);

    if (!options.screenshot.isEmpty()) {
        window.setRememberSession(false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 860);
        scheduleScreenshot(window, options);
    } else if (!options.select.isEmpty()) {
        QObject::connect(&window, &viewer::app::MainWindow::sessionChanged, &window,
                         [&window, sel = options.select] { window.selectPath(sel); });
    }
    window.show();

    // `26Viewer <folder|archive>` opens directly (also used by "Open with").
    // Otherwise reopen last time's game, falling back to the game picker.
    if (!options.path.isEmpty())
        window.openPath(options.path);
    else if (options.screenshot.isEmpty())
        QTimer::singleShot(0, &window, [&window] {
            if (!window.reopenLastSession())
                window.chooseGame();
        });
    else // screenshot of what start-up shows: the reopened game, or the empty start screen
        QTimer::singleShot(0, &window, [&window] {
            if (!window.reopenLastSession())
                emit window.sessionChanged();
        });

    return QApplication::exec();
}
