// 26Viewer — Qt desktop front-end.

#include "MainWindow.hpp"
#include "PreviewWindow.hpp"
#include "TextEditorWindow.hpp"
#include "TextureBrowser.hpp"
#include "Theme.hpp"
#include "preview/PreviewPane.hpp"

#include "games/GameModules.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QTimer>

#include <memory>

namespace {

struct Options {
    QString path;
    QString select;
    QString screenshot;
    QString theme;
    QString animation; ///< Virtual path of an animation to pose the model with ("path@frame").
    QString camera;    ///< --camera: "yaw,pitch,zoom[,x,y,z]".
    QString dialog;    ///< --dialog: a window or menu to picture instead (goto, recent, settings).
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
                                 QStringLiteral("Preview tab to show: preview, structure, hex or properties "
                                                "(or edit: open a text file in the editor; textures: a model's "
                                                "Textures window)."),
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
    const QCommandLineOption camera(QStringLiteral("camera"),
                                    QStringLiteral("3D view for the screenshot: yaw,pitch,zoom[,x,y,z] (degrees; "
                                                   "zoom 1 = the whole model; the point to look at)."),
                                    QStringLiteral("view"));
    const QCommandLineOption animation(QStringLiteral("animation"),
                                       QStringLiteral("Pose the selected model with this animation (virtual path, "
                                                      "optionally @frame)."),
                                       QStringLiteral("path"));
    const QCommandLineOption dialog(QStringLiteral("dialog"),
                                    QStringLiteral("Screenshot of a dialog or menu instead: goto, recent or settings."),
                                    QStringLiteral("name"));
    parser.addOptions({select, tab, theme, screenshot, editMode, animation, camera, dialog});
    parser.process(app);

    Options o;
    o.path = parser.positionalArguments().value(0);
    o.select = parser.value(select);
    o.screenshot = parser.value(screenshot);
    o.theme = parser.value(theme).toLower();
    o.editMode = parser.isSet(editMode);
    o.animation = parser.value(animation);
    o.camera = parser.value(camera);
    o.dialog = parser.value(dialog).toLower();
    const QStringList tabs{"preview", "structure", "hex", "properties", "edit", "textures", "animations"};
    o.tab = tabs.indexOf(parser.value(tab).toLower());
    return o;
}

/// --screenshot: once loaded, grab the main window — or, when --select names
/// a file, its preview window once the file has loaded — and quit.
void scheduleScreenshot(viewer::app::MainWindow& window, const Options& o)
{
    auto capture = [file = o.screenshot](QWidget* target) {
        QTimer::singleShot(400, target, [target, file] {
            target->grab().save(file);
            QApplication::quit();
        });
    };
    constexpr int kEditTab = 4, kTexturesTab = 5, kAnimationsTab = 6;
    QObject::connect(&window, &viewer::app::MainWindow::sessionChanged, &window, [&window, o, capture] {
        if (!o.dialog.isEmpty()) { // the dialog (or menu) itself, once it is up
            if (!o.select.isEmpty())
                window.selectPath(o.select, /*openFile=*/false);
            QTimer::singleShot(1200, &window, [file = o.screenshot] {
                QWidget* shown = QApplication::activeModalWidget();
                if (!shown)
                    shown = QApplication::activePopupWidget();
                if (shown) {
                    shown->grab().save(file);
                    shown->close();
                }
                QApplication::quit();
            });
            QTimer::singleShot(0, &window, [&window, name = o.dialog] { window.showForScreenshot(name); });
            return;
        }
        auto opened = std::make_shared<bool>(false);
        QObject::connect(&window, &viewer::app::MainWindow::previewOpened, &window,
                         [&window, o, capture, opened](viewer::app::PreviewWindow* preview) {
                             *opened = true;
                             preview->resize(1100, 800);
                             auto* pane = preview->pane();
                             QObject::connect(pane, &viewer::app::PreviewPane::previewReady, preview,
                                              [&window, pane, preview, o, capture] {
                                                  if (o.tab == kEditTab) { // the text editor's picture instead
                                                      QObject::connect(&window, &viewer::app::MainWindow::editorOpened,
                                                                       &window,
                                                                       [capture](QWidget* editor) {
                                                                           editor->resize(1100, 800);
                                                                           capture(editor);
                                                                       },
                                                                       Qt::SingleShotConnection);
                                                      emit pane->editRequested();
                                                      return;
                                                  }
                                                  if (o.tab == kTexturesTab) { // the Textures window instead
                                                      preview->showTextures();
                                                      if (viewer::app::TextureBrowser* b = preview->textureBrowser()) {
                                                          b->resize(1100, 720);
                                                          QTimer::singleShot(1500, b, [b, capture] { capture(b); });
                                                      }
                                                      return;
                                                  }
                                                  if (o.tab == kAnimationsTab) { // the animation picker instead
                                                      QTimer::singleShot(3000, preview, [] {
                                                          if (QWidget* picker = QApplication::activeModalWidget())
                                                              picker->grab().save(qApp->property("shotFile").toString());
                                                          QApplication::quit();
                                                      });
                                                      QTimer::singleShot(0, pane, [pane] { pane->chooseAnimation(); });
                                                      return;
                                                  }
                                                  if (o.tab >= 0)
                                                      pane->setCurrentTab(o.tab);
                                                  if (!o.camera.isEmpty())
                                                      pane->setCamera(o.camera);
                                                  if (!o.animation.isEmpty()) {
                                                      const qsizetype at = o.animation.lastIndexOf(QLatin1Char('@'));
                                                      pane->showAnimation(at > 0 ? o.animation.left(at) : o.animation,
                                                                          at > 0 ? o.animation.mid(at + 1).toFloat() : 0.0f);
                                                  }
                                                  capture(preview);
                                              },
                                              Qt::SingleShotConnection);
                         },
                         Qt::SingleShotConnection);
        if (o.select.isEmpty() || !window.selectPath(o.select) || !*opened)
            capture(&window); // nothing selected, or a folder: the main window is the picture
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

    app.setProperty("shotFile", options.screenshot);
    if (!options.screenshot.isEmpty()) {
        window.setRememberSession(false);
        window.setOffscreen(true);
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
