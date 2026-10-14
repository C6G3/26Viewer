#include "MainWindow.hpp"

#include "BackupBanner.hpp"
#include "BusyOverlay.hpp"
#include "Exporter.hpp"
#include "FolderView.hpp"
#include "GamePickerDialog.hpp"
#include "Icons.hpp"
#include "ImportExportDialogs.hpp"
#include "QtConvert.hpp"
#include "Theme.hpp"
#include "VfsModel.hpp"
#include "PreviewWindow.hpp"
#include "TextureBrowser.hpp"
#include "TextEditorWindow.hpp"
#include "preview/PreviewPane.hpp"

#include "viewer/core/ContentRegistry.hpp"
#include "viewer/core/RandomAccessFile.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace viewer::app {

namespace {

QString appName()
{
    return QApplication::applicationDisplayName();
}

/// Runs `job` on a worker thread while a small "please wait" dialog shows
/// (for re-encoding sounds and compressing textures, which take a moment).
template <typename T>
T runBusy(QWidget* parent, const QString& text, std::function<T()> job)
{
    QProgressDialog dialog(text, QString(), 0, 0, parent);
    dialog.setWindowTitle(QApplication::applicationDisplayName());
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setCancelButton(nullptr);
    dialog.setMinimumDuration(400);
    QFutureWatcher<T> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<T>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(std::move(job)));
    if (!watcher.isFinished())
        loop.exec();
    return watcher.result();
}

QString sizeText(std::uint64_t bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

/// Walks `path` ("a/b/c") down from `start`, ignoring case.
const VfsNode* walk(const VfsNode* start, const QString& path)
{
    const VfsNode* node = start;
    for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const VfsNode* next = nullptr;
        for (const auto& child : node->children())
            if (toQString(child->name()).compare(part, Qt::CaseInsensitive) == 0)
                next = child.get();
        if (!next)
            return nullptr;
        node = next;
    }
    return node;
}

} // namespace

MainWindow::MainWindow(const GameRegistry& games, ThemeManager& theme, QWidget* parent)
    : QMainWindow(parent), m_games(games), m_theme(theme), m_loadWatcher(new QFutureWatcher<LoadResult>(this)),
      m_extractWatcher(new QFutureWatcher<ExtractSummary>(this)),
      m_editWatcher(new QFutureWatcher<EditResult>(this)), m_model(new VfsModel(this)),
      m_treeFilter(new VfsFilterModel(this)), m_leftStack(nullptr), m_tree(nullptr), m_placeholder(nullptr),
      m_folderView(nullptr), m_search(nullptr), m_statusLabel(new QLabel(this)), m_progress(new QProgressBar(this)),
      m_cancel(new QToolButton(this)), m_mountWatcher(new QFutureWatcher<MountResult>(this))
{
    setWindowTitle(appName());
    setWindowIcon(icons::appIcon());
    setAcceptDrops(true);
    m_treeFilter->setSourceModel(m_model);

    buildActions();
    buildUi();

    connect(m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::onLoadFinished);
    connect(m_mountWatcher, &QFutureWatcher<MountResult>::finished, this, &MainWindow::onMountFinished);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::finished, this, &MainWindow::onExtractFinished);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::progressRangeChanged, m_progress,
            &QProgressBar::setRange);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::progressValueChanged, m_progress,
            &QProgressBar::setValue);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::progressTextChanged, this,
            [this](const QString& text) {
                m_statusLabel->setText((m_exporting ? tr("Exporting %1…") : tr("Extracting %1…")).arg(text));
            });
    connect(m_editWatcher, &QFutureWatcher<EditResult>::finished, this, &MainWindow::onEditFinished);
    // Which pane has focus decides what "selected" means (see selectedNodes).
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::updateActions);

    QSettings settings;
    if (!restoreGeometry(settings.value("window/geometry").toByteArray()))
        resize(1280, 800);
    restoreState(settings.value("window/state").toByteArray());
    updateActions();
}

MainWindow::~MainWindow()
{
    for (PreviewWindow* w : previewWindows()) // top-level windows without a parent
        delete w;
    for (TextEditorWindow* w : editorWindows())
        delete w;
    m_loadWatcher->waitForFinished();
    m_mountWatcher->waitForFinished();
    m_extractWatcher->waitForFinished();
    m_editWatcher->waitForFinished();
}

// ---- Construction ------------------------------------------------------------------------

void MainWindow::buildActions()
{
    using TI = QIcon::ThemeIcon;

    m_openGameAction = new QAction(icons::theme(TI::InputGaming, QStyle::SP_DirOpenIcon), tr("Open &Game…"), this);
    m_openGameAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    m_openGameAction->setStatusTip(tr("Choose a game and its install folder"));
    connect(m_openGameAction, &QAction::triggered, this, &MainWindow::chooseGame);

    m_openArchiveAction =
        new QAction(icons::theme(TI::DocumentOpen, QStyle::SP_DialogOpenButton), tr("Open &Archive…"), this);
    m_openArchiveAction->setShortcut(QKeySequence::Open);
    m_openArchiveAction->setStatusTip(tr("Open an archive (added alongside the open game)"));
    connect(m_openArchiveAction, &QAction::triggered, this, &MainWindow::openArchiveDialog);

    m_openFolderAction = new QAction(icons::theme(TI::FolderOpen, QStyle::SP_DirOpenIcon), tr("Open &Folder…"), this);
    m_openFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_O));
    m_openFolderAction->setStatusTip(tr("Open any folder — a mod workspace, say — alongside the open game"));
    connect(m_openFolderAction, &QAction::triggered, this, &MainWindow::openFolderDialog);

    m_extractAction =
        new QAction(icons::theme(TI::DocumentSaveAs, QStyle::SP_DialogSaveButton), tr("&Extract Selected…"), this);
    m_extractAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(m_extractAction, &QAction::triggered, this, [this] { extract(false); });

    m_extractAllAction = new QAction(icons::theme(TI::DocumentSave, QStyle::SP_DialogSaveButton), tr("Extract A&ll…"), this);
    m_extractAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(m_extractAllAction, &QAction::triggered, this, [this] { extract(true); });

    m_exportAction = new QAction(icons::theme(TI::DocumentSend, QStyle::SP_DialogSaveButton), tr("E&xport…"), this);
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_E));
    m_exportAction->setStatusTip(tr("Save the selection converted for other programs: sound banks → folders of "
                                    ".wav, models → .obj, textures → .png / .dds / .jpg / .bmp"));
    connect(m_exportAction, &QAction::triggered, this, [this] { exportNodes(selectedNodes()); });

    m_importAction = new QAction(icons::theme(TI::DocumentOpen, QStyle::SP_DialogOpenButton), tr("&Import…"), this);
    m_importAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_importAction->setStatusTip(tr("Convert your files back into the selected one: a folder of .wav files into a "
                                    "sound bank, a picture into a texture. Needs Edit Mode."));
    connect(m_importAction, &QAction::triggered, this, [this] {
        if (const auto nodes = selectedNodes(); nodes.size() == 1)
            importNode(nodes.front());
    });

    m_copyPathAction = new QAction(icons::theme(TI::EditCopy, QStyle::SP_FileIcon), tr("&Copy Path"), this);
    m_copyPathAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    connect(m_copyPathAction, &QAction::triggered, this, [this] {
        QStringList paths;
        for (const VfsNode* n : selectedNodes())
            paths << toQString(n->virtualPath());
        QGuiApplication::clipboard()->setText(paths.join('\n'));
    });

    m_revealAction = new QAction(icons::theme(TI::FolderNew, QStyle::SP_DirIcon), tr("Show in E&xplorer"), this);
    connect(m_revealAction, &QAction::triggered, this, [this] {
        const auto nodes = selectedNodes();
        if (nodes.empty() || nodes.front()->diskPath().empty())
            return;
        const QString native = QDir::toNativeSeparators(toQString(nodes.front()->diskPath()));
#ifdef Q_OS_WIN
        QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), native});
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(native).absolutePath()));
#endif
    });

    m_replaceAction =
        new QAction(icons::theme(TI::DocumentRevert, QStyle::SP_BrowserReload), tr("&Replace…"), this);
    m_replaceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    m_replaceAction->setStatusTip(tr("Replace the selected file with one of your own (same file type). "
                                     "You can also drag a file from Explorer onto an item. Needs Edit Mode."));
    connect(m_replaceAction, &QAction::triggered, this, &MainWindow::replaceSelected);

    m_restoreAction = new QAction(icons::theme(TI::EditUndo, QStyle::SP_ArrowBack), tr("Restore &Original…"), this);
    m_restoreAction->setStatusTip(
        tr("Put back the original from the backup: one file inside an archive, or a whole archive / loose file"));
    connect(m_restoreAction, &QAction::triggered, this, &MainWindow::restoreOriginal);

    m_editTextAction = new QAction(icons::theme(TI::DocumentProperties, QStyle::SP_FileDialogDetailedView),
                                   tr("&Edit Text…"), this);
    m_editTextAction->setShortcut(QKeySequence(Qt::Key_F4));
    m_editTextAction->setStatusTip(tr("Open the selected text file in the text editor (saving needs Edit Mode)"));
    connect(m_editTextAction, &QAction::triggered, this, [this] {
        if (const auto nodes = selectedNodes(); nodes.size() == 1)
            openTextEditor(nodes.front());
    });

    m_editModeAction = new QAction(icons::editMode(), tr("Edit &Mode"), this);
    m_editModeAction->setCheckable(true);
    m_editModeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    m_editModeAction->setStatusTip(tr("Unlock replacing and restoring game files"));
    connect(m_editModeAction, &QAction::triggered, this, [this](bool on) { setEditMode(on); });

    m_openAction = new QAction(icons::theme(TI::DocumentOpen, QStyle::SP_DialogOpenButton), tr("&Open"), this);
    connect(m_openAction, &QAction::triggered, this, [this] {
        if (const auto nodes = selectedNodes(); nodes.size() == 1)
            activateNode(nodes.front());
    });

    m_propertiesAction = new QAction(icons::theme(TI::DocumentProperties, QStyle::SP_FileDialogInfoView),
                                     tr("P&roperties"), this);
    m_propertiesAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    connect(m_propertiesAction, &QAction::triggered, this, [this] {
        if (const auto nodes = selectedNodes(); nodes.size() == 1)
            openPreview(nodes.front(), /*properties=*/true);
    });

    // Backspace / Alt+Left: preview → folder list → parent folder. Scoped to
    // the right-hand pane so Backspace still edits the search box.
    m_backAction = new QAction(icons::theme(TI::GoPrevious, QStyle::SP_ArrowBack), tr("&Back"), this);
    m_backAction->setShortcuts({QKeySequence(Qt::Key_Backspace), QKeySequence(QKeySequence::Back)});
    m_backAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_backAction, &QAction::triggered, this, &MainWindow::goBack);

    m_reopenAction = new QAction(tr("Reopen Last Game at &Startup"), this);
    m_reopenAction->setCheckable(true);
    m_reopenAction->setChecked(QSettings().value("session/reopenOnStartup", true).toBool());
    connect(m_reopenAction, &QAction::toggled, this,
            [](bool on) { QSettings().setValue("session/reopenOnStartup", on); });

    // Menus
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_openGameAction);
    file->addAction(m_openArchiveAction);
    file->addAction(m_openFolderAction);
    file->addSeparator();
    file->addAction(m_extractAction);
    file->addAction(m_extractAllAction);
    file->addAction(m_exportAction);
    file->addSeparator();
    file->addAction(m_reopenAction);
    file->addSeparator();
    QAction* quit = file->addAction(icons::theme(TI::ApplicationExit, QStyle::SP_DialogCloseButton), tr("E&xit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_editModeAction);
    edit->addSeparator();
    edit->addAction(m_replaceAction);
    edit->addAction(m_importAction);
    edit->addAction(m_editTextAction);
    edit->addAction(m_restoreAction);
    edit->addSeparator();
    edit->addAction(m_copyPathAction);
    edit->addAction(m_revealAction);
    edit->addSeparator();
    QAction* find = edit->addAction(icons::theme(TI::EditFind, QStyle::SP_FileDialogContentsView), tr("&Find…"));
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });

    auto* showFiles = new QAction(tr("Show &Files in Tree"), this);
    showFiles->setCheckable(true);
    showFiles->setChecked(QSettings().value("view/treeShowsFiles", true).toBool());
    showFiles->setStatusTip(tr("Off: the tree lists only folders and archives, like OpenIV"));
    m_treeFilter->setContainersOnly(!showFiles->isChecked());
    connect(showFiles, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("view/treeShowsFiles", on);
        m_treeFilter->setContainersOnly(!on);
    });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(showFiles);
    view->addSeparator();
    view->addAction(m_backAction);
    view->addAction(m_propertiesAction);
    view->addSeparator();
    QAction* expand = view->addAction(tr("&Expand All"));
    connect(expand, &QAction::triggered, this, [this] { m_tree->expandAll(); });
    QAction* collapse = view->addAction(tr("&Collapse All"));
    connect(collapse, &QAction::triggered, this, [this] { m_tree->collapseAll(); });
    view->addSeparator();
    m_theme.populateMenu(view->addMenu(tr("&Theme")));

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    QAction* about = help->addAction(icons::theme(TI::HelpAbout, QStyle::SP_MessageBoxInformation),
                                     tr("&About %1").arg(appName()));
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

QWidget* MainWindow::buildPlaceholder()
{
    auto* page = new QWidget(this);
    auto* title = new QLabel(tr("No game open"), page);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.6);
    f.setWeight(QFont::DemiBold);
    title->setFont(f);
    auto* text = new QLabel(tr("Choose a game to browse its archives, or drop an archive file here."), page);
    text->setForegroundRole(QPalette::PlaceholderText);
    text->setWordWrap(true);
    text->setAlignment(Qt::AlignCenter);

    auto* gameButton = new QPushButton(m_openGameAction->icon(), tr("Open Game…"), page);
    gameButton->setDefault(true);
    connect(gameButton, &QPushButton::clicked, m_openGameAction, &QAction::trigger);
    auto* archiveButton = new QPushButton(m_openArchiveAction->icon(), tr("Open Archive…"), page);
    connect(archiveButton, &QPushButton::clicked, m_openArchiveAction, &QAction::trigger);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(gameButton);
    buttons->addWidget(archiveButton);
    buttons->addStretch();

    auto* layout = new QVBoxLayout(page);
    layout->addStretch();
    layout->addWidget(title, 0, Qt::AlignHCenter);
    layout->addWidget(text);
    layout->addSpacing(12);
    layout->addLayout(buttons);
    layout->addStretch();
    return page;
}

void MainWindow::buildUi()
{
    // Toolbar
    QToolBar* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName("mainToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(m_openGameAction);
    toolbar->addAction(m_openArchiveAction);
    toolbar->addAction(m_openFolderAction);
    toolbar->addSeparator();
    toolbar->addAction(m_extractAction);
    toolbar->addAction(m_extractAllAction);
    toolbar->addAction(m_exportAction);
    toolbar->addSeparator();
    toolbar->addAction(m_replaceAction);
    toolbar->addAction(m_importAction);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Edit Mode stands apart on the right, next to search, so its state is always obvious.
    toolbar->addAction(m_editModeAction);
    auto* gap = new QWidget(toolbar);
    gap->setFixedWidth(12);
    toolbar->addWidget(gap);

    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Search this folder  (Ctrl+F)"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(260);
    m_search->addAction(icons::theme(QIcon::ThemeIcon::SystemSearch, QStyle::SP_FileDialogContentsView),
                        QLineEdit::LeadingPosition);
    toolbar->addWidget(m_search);

    // Debounced filtering keeps typing smooth on large trees.
    auto* filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, filterTimer, qOverload<>(&QTimer::start));
    connect(filterTimer, &QTimer::timeout, this, [this] {
        m_folderView->setFilterText(m_search->text().trimmed());
        updateActions();
    });

    // Navigation tree (optionally folders and archives only: View ▸ Show Files in Tree).
    m_tree = new QTreeView(this);
    m_tree->setModel(m_treeFilter);
    m_tree->setHeaderHidden(true);
    for (int column = VfsModel::Type; column < VfsModel::ColumnCount; ++column)
        m_tree->setColumnHidden(column, true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(VfsModel::Name, Qt::AscendingOrder);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &MainWindow::showTreeContextMenu);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        // A folder opens in the list; a file shows its folder with the file selected.
        const VfsNode* node = VfsModel::nodeFromIndex(m_treeFilter->mapToSource(index));
        const VfsNode* folder = node && node->isFile() ? node->parent() : node;
        if (folder != m_folderView->folder())
            m_folderView->setFolder(folder);
        if (node && node->isFile())
            m_folderView->selectNode(node);
        updateBackupBanner();
        updateActions();
    });
    connect(m_tree, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (const VfsNode* node = VfsModel::nodeFromIndex(m_treeFilter->mapToSource(index)); node && node->isFile())
            openPreview(node); // folders just expand on double-click
    });
    // Files dragged from Explorer onto a file in the tree replace it too.
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->viewport()->installEventFilter(this);

    m_placeholder = buildPlaceholder();
    m_leftStack = new QStackedWidget(this);
    m_leftStack->addWidget(m_placeholder);
    m_leftStack->addWidget(m_tree);

    // Right: the open folder's contents; double-click a file to open it in a preview window.
    m_folderView = new FolderView(m_model, this);
    m_folderView->setPlaceholder(QString(), tr("Open a game or an archive to see its contents here."));
    m_folderView->restoreHeaderState(QSettings().value("window/listHeader").toByteArray());
    QTreeView* list = m_folderView->view();
    // Files dragged from Explorer onto an item replace it (see eventFilter).
    list->setAcceptDrops(true);
    list->viewport()->setAcceptDrops(true);
    list->viewport()->installEventFilter(this);
    connect(m_folderView, &FolderView::activated, this, &MainWindow::activateNode);
    connect(m_folderView, &FolderView::navigateRequested, this,
            [this](const VfsNode* folder) { navigateTo(folder, m_folderView->folder()); });
    connect(m_folderView, &FolderView::selectionChanged, this, &MainWindow::updateActions);
    connect(m_folderView, &FolderView::contextMenuRequested, this, &MainWindow::showListContextMenu);
    connect(m_folderView->banner(), &BackupBanner::backupRequested, this,
            [this] { backupNode(m_folderView->folder()); });
    m_folderView->addAction(m_backAction);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("mainSplitter");
    splitter->addWidget(m_leftStack);
    splitter->addWidget(m_folderView);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    if (!splitter->restoreState(QSettings().value("window/browserSplitter").toByteArray()))
        splitter->setSizes({340, 1060});
    setCentralWidget(splitter);
    m_busy = new BusyOverlay(splitter);

    // Status bar
    m_progress->setMaximumWidth(220);
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_cancel->setIcon(icons::theme(QIcon::ThemeIcon::ProcessStop, QStyle::SP_BrowserStop));
    m_cancel->setToolTip(tr("Cancel"));
    m_cancel->setAutoRaise(true);
    m_cancel->hide();
    connect(m_cancel, &QToolButton::clicked, this, [this] { m_extractWatcher->cancel(); });
    m_statusLabel->setTextFormat(Qt::RichText);
    m_statusLabel->setOpenExternalLinks(false);
    connect(m_statusLabel, &QLabel::linkActivated, this,
            [](const QString& link) { QDesktopServices::openUrl(QUrl(link)); });
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_progress);
    statusBar()->addPermanentWidget(m_cancel);
    m_statusLabel->setText(tr("Ready"));

    // Always-visible reminder while files can be modified.
    m_editModeBadge = new QLabel(tr("EDIT MODE"), this);
    m_editModeBadge->setStyleSheet(QStringLiteral(
        "QLabel { background: #C42B1C; color: white; font-weight: 700; border-radius: 4px; padding: 2px 8px; }"));
    m_editModeBadge->setToolTip(tr("Game files can be modified. Turn off with Edit ▸ Edit Mode (Ctrl+M)."));
    m_editModeBadge->hide();
    statusBar()->addPermanentWidget(m_editModeBadge);

}

// ---- Opening -------------------------------------------------------------------------------

bool MainWindow::chooseGame()
{
    GamePickerDialog dialog(m_games, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    if (dialog.wantsArchive()) {
        openArchiveDialog();
        return true;
    }
    if (const Game* game = dialog.selectedGame())
        openGame(*game, dialog.installPath());
    return true;
}

void MainWindow::openPath(const QString& path)
{
    const QFileInfo info(path);
    if (info.isDir())
        openFolder(toPath(info.absoluteFilePath()));
    else if (info.isFile())
        openArchive(toPath(info.absoluteFilePath()));
    else
        QMessageBox::warning(this, appName(), tr("%1 does not exist.").arg(QDir::toNativeSeparators(path)));
}

void MainWindow::openGame(const Game& game, const std::filesystem::path& directory)
{
    // Each game remembers the archives/folders opened alongside it.
    std::vector<std::filesystem::path> extras;
    for (const QString& extra : QSettings().value(extrasKey(&game)).toStringList())
        extras.push_back(toPath(extra));
    openSource(&game, directory, std::move(extras), {});
}

QString MainWindow::extrasKey(const Game* game)
{
    return game ? QStringLiteral("games/%1/extras").arg(toQString(game->info().id))
                : QStringLiteral("session/extras");
}

void MainWindow::openSource(const Game* game, const std::filesystem::path& source,
                            std::vector<std::filesystem::path> extras, const QString& select)
{
    const GameRegistry* games = &m_games;
    const QString name = game ? toQString(game->info().title) : toQString(source.filename());
    m_pendingSelect = select;
    startLoad(tr("Opening %1…").arg(name), [games, game, source, extras = std::move(extras)] {
        LoadResult result;
        std::error_code ec;
        if (game) {
            result.session = openGameSession(*game, source);
        } else if (std::filesystem::is_directory(source, ec)) {
            result.session = openFolderSession(*games, source);
        } else {
            auto session = openArchiveSession(*games, source);
            if (!session)
                return LoadResult{nullptr, toQString(session.error().message), {}};
            result.session = std::move(*session);
        }
        // Remount what was open alongside it (after a reload or at start-up).
        for (const auto& path : extras) {
            if (!std::filesystem::exists(path, ec))
                continue;
            if (auto extra = mountExtra(result.session->content, path))
                result.session->extras.push_back(std::move(*extra));
            else
                result.warnings << toQString(extra.error().message);
        }
        return result;
    });
}

void MainWindow::openArchiveDialog()
{
    QSettings settings;
    QStringList patterns;
    const auto addPatterns = [&](const ContentRegistry& content) {
        for (const auto& format : content.archiveFormats())
            for (const auto& ext : format->extensions())
                patterns << QStringLiteral("*.%1").arg(toQString(ext));
    };
    if (m_session) { // only archives the open game understands
        addPatterns(m_session->content);
    } else {
        for (const auto& g : m_games.games()) {
            ContentRegistry probe;
            g->registerContent(probe);
            addPatterns(probe);
        }
    }
    patterns.removeDuplicates();
    const QString filter = tr("Archives (%1);;All files (*)").arg(patterns.join(' '));
    const QString title = m_session ? tr("Open Archive alongside %1").arg(m_session->title()) : tr("Open Archive");
    const QString file =
        QFileDialog::getOpenFileName(this, title, settings.value("paths/lastArchiveDir").toString(), filter);
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastArchiveDir", QFileInfo(file).absolutePath());
    openArchive(toPath(file));
}

void MainWindow::openFolderDialog()
{
    QSettings settings;
    const QString title = m_session ? tr("Open Folder alongside %1").arg(m_session->title()) : tr("Open Folder");
    const QString dir = QFileDialog::getExistingDirectory(this, title, settings.value("paths/lastFolderDir").toString());
    if (dir.isEmpty())
        return;
    settings.setValue("paths/lastFolderDir", dir);
    openFolder(toPath(dir));
}

void MainWindow::openArchive(const std::filesystem::path& file)
{
    if (m_session)
        addMount(file);
    else
        openSource(nullptr, file, {}, {});
}

void MainWindow::openFolder(const std::filesystem::path& directory)
{
    if (const Game* game = m_games.detect(directory)) { // a game install opens as that game
        openGame(*game, directory);
        return;
    }
    if (m_session)
        addMount(directory);
    else
        openSource(nullptr, directory, {}, {});
}

// ---- Archives / folders opened alongside the game -------------------------------------------

void MainWindow::addMount(const std::filesystem::path& source)
{
    if (!m_session || m_mountWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    // Already open? Just go there.
    std::error_code ec;
    for (std::size_t i = 0; i < m_session->extras.size(); ++i) {
        if (std::filesystem::equivalent(m_session->extras[i]->source, source, ec)) {
            navigateTo(m_session->extras[i]->top);
            return;
        }
    }
    if (std::filesystem::equivalent(m_session->source, source, ec)) {
        navigateTo(&m_session->vfs.root());
        return;
    }

    m_statusLabel->setText(tr("Opening %1…").arg(toQString(source.filename())));
    m_progress->setRange(0, 0);
    m_progress->show();
    m_busy->start(tr("Opening %1…").arg(toQString(source.filename())));
    QApplication::setOverrideCursor(Qt::BusyCursor);
    // The job keeps the session (and so its content handlers) alive.
    m_mountWatcher->setFuture(QtConcurrent::run([session = m_session, source] {
        MountResult result;
        result.session = session.get();
        if (auto extra = mountExtra(session->content, source))
            result.extra = std::move(*extra);
        else
            result.error = toQString(extra.error().message);
        return result;
    }));
    updateActions();
}

void MainWindow::onMountFinished()
{
    m_busy->stop();
    QApplication::restoreOverrideCursor();
    m_progress->hide();
    MountResult result = m_mountWatcher->result();
    if (!result.extra) {
        m_statusLabel->setText(tr("Open failed"));
        QMessageBox::warning(this, appName(), tr("Could not open:\n%1").arg(result.error));
    } else if (m_session && m_session.get() == result.session) { // still the same session
        const Session::Extra& extra = *result.extra;
        m_session->extras.push_back(result.extra);
        m_model->appendRoot(extraRoot(extra));
        navigateTo(extra.top);
        m_tree->expand(m_treeFilter->mapFromSource(m_model->indexForNode(extra.top)));
        m_statusLabel->setText(tr("Opened %1  ·  %n file(s)", nullptr, static_cast<int>(extra.top->fileCount()))
                                   .arg(toQString(extra.source.filename())));
        rememberSession();
    }
    updateActions();
}

void MainWindow::closeMount(const VfsNode* top)
{
    if (!m_session || m_editWatcher->isRunning() || m_extractWatcher->isRunning())
        return;
    const auto it = std::ranges::find_if(m_session->extras, [top](const auto& e) { return e->top == top; });
    if (it == m_session->extras.end())
        return;
    // Let go of everything that points into it before it is unmounted.
    for (PreviewWindow* w : previewWindows()) {
        if (w->node() && m_model->topOf(w->node()) == top) {
            w->setSession(nullptr); // waits for a running load
            w->close();
        }
    }
    for (TextEditorWindow* w : editorWindows())
        if (w->node() && m_model->topOf(w->node()) == top)
            w->rebind(nullptr, nullptr);
    if (m_folderView->folder() && m_model->topOf(m_folderView->folder()) == top)
        navigateTo(&m_session->vfs.root());
    m_model->removeRoot(m_model->rootRow(top));
    m_session->extras.erase(it);
    rememberSession();
    updateActions();
}

void MainWindow::startLoad(const QString& message, std::function<LoadResult()> job)
{
    if (m_loadWatcher->isRunning())
        return;
    m_statusLabel->setText(message);
    m_progress->setRange(0, 0); // indeterminate
    m_progress->show();
    m_busy->start(message, 100);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_loadWatcher->setFuture(QtConcurrent::run(std::move(job)));
    updateActions();
}

void MainWindow::onLoadFinished()
{
    m_busy->stop();
    QApplication::restoreOverrideCursor();
    m_progress->hide();
    const LoadResult result = m_loadWatcher->result();
    if (!result.session) {
        m_statusLabel->setText(tr("Open failed"));
        QMessageBox::warning(this, appName(), tr("Could not open the file:\n%1").arg(result.error));
        updateActions();
        return;
    }
    setSession(result.session);
    QStringList lines = result.warnings;
    for (const auto& w : result.session->report.warnings)
        lines << toQString(w.message);
    if (!lines.isEmpty()) {
        QMessageBox::warning(this, appName(),
                             tr("Some archives could not be opened:\n\n%1").arg(lines.mid(0, 15).join('\n')));
    }
}

void MainWindow::setSession(std::shared_ptr<Session> session)
{
    // Preview windows let go of the old session's files first (they may be
    // about to be restored or rewritten), then follow their file into the new one.
    for (PreviewWindow* w : previewWindows())
        w->setSession(nullptr);
    for (TextEditorWindow* w : editorWindows())
        w->rebind(nullptr, nullptr);
    m_model->setRoots({}, nullptr);
    m_session = std::move(session);
    if (m_session) {
        std::vector<VfsModel::Root> roots{mainRoot(*m_session)};
        for (const auto& extra : m_session->extras)
            roots.push_back(extraRoot(*extra));
        m_model->setRoots(std::move(roots), &m_session->content);
        for (PreviewWindow* w : previewWindows()) {
            if (const VfsNode* node = findLocation(w->location())) {
                w->setSession(m_session);
                w->showNode(node);
                w->setLocation(locationOf(node));
            } else {
                w->close(); // the file is gone (or another game was opened)
            }
        }
        // Editors keep their text either way; Save needs the file to still be there.
        for (TextEditorWindow* w : editorWindows())
            w->rebind(m_session, findLocation(w->location()));
        m_leftStack->setCurrentWidget(m_tree);
        m_tree->expand(m_treeFilter->index(0, 0)); // the game's top level
        m_tree->setCurrentIndex(m_treeFilter->index(0, 0));
        setWindowTitle(QStringLiteral("%1 — %2").arg(m_session->title(), appName()));
        const VfsNode& root = m_session->vfs.root();
        m_statusLabel->setText(tr("%1  ·  %n archive(s)", nullptr, static_cast<int>(m_session->report.archives))
                                   .arg(m_session->title())
                               + tr("  ·  %n file(s)", nullptr, static_cast<int>(root.fileCount()))
                               + QStringLiteral("  ·  %1").arg(sizeText(root.size())));
        rememberSession();
        if (!m_pendingSelect.isEmpty()) {
            selectPath(m_pendingSelect, /*openFile=*/false);
            m_pendingSelect.clear();
        }
    } else {
        m_leftStack->setCurrentWidget(m_placeholder);
        setWindowTitle(appName());
        m_statusLabel->setText(tr("Ready"));
    }
    updateBackupBanner();
    updateActions();
    emit sessionChanged();
}

QString MainWindow::locationOf(const VfsNode* node) const
{
    if (!m_session || !node)
        return {};
    // Inside the main mount: just the virtual path. Inside an extra mount:
    // "<its source on disk>|<virtual path>" ('|' can't occur in Windows paths).
    const VfsNode* top = m_model->topOf(node);
    for (const auto& extra : m_session->extras)
        if (extra->top == top)
            return toQString(extra->source) + QLatin1Char('|') + toQString(node->virtualPath());
    return toQString(node->virtualPath());
}

const VfsNode* MainWindow::findLocation(const QString& location) const
{
    if (!m_session)
        return nullptr;
    const qsizetype bar = location.indexOf(QLatin1Char('|'));
    if (bar < 0)
        return walk(&m_session->vfs.root(), location);
    const auto source = toPath(location.left(bar));
    for (const auto& extra : m_session->extras)
        if (extra->source == source)
            return walk(&extra->vfs->root(), location.mid(bar + 1));
    return nullptr;
}

bool MainWindow::selectPath(const QString& location, bool openFile)
{
    const VfsNode* node = findLocation(location);
    if (!node)
        return false;
    if (node->isContainer()) {
        navigateTo(node);
        return true;
    }
    navigateTo(node->parent());
    m_folderView->selectNode(node);
    if (openFile)
        openPreview(node);
    return true;
}

// ---- Browsing ----------------------------------------------------------------------------------

const VfsNode* MainWindow::treeNode() const
{
    return VfsModel::nodeFromIndex(m_treeFilter->mapToSource(m_tree->currentIndex()));
}

void MainWindow::navigateTo(const VfsNode* folder, const VfsNode* reveal)
{
    const QModelIndex index = m_treeFilter->mapFromSource(m_model->indexForNode(folder));
    if (!index.isValid())
        return;
    m_tree->setCurrentIndex(index); // updates the list (see buildUi)
    m_tree->scrollTo(index);        // also expands the tree down to it

    // Coming back up: highlight the child we came from, like Explorer does.
    while (reveal && reveal->parent() != folder)
        reveal = reveal->parent();
    if (reveal)
        m_folderView->selectNode(reveal);
}

void MainWindow::activateNode(const VfsNode* node)
{
    if (!node)
        return;
    if (node->isContainer())
        navigateTo(node);
    else
        openPreview(node);
}

std::vector<PreviewWindow*> MainWindow::previewWindows() const
{
    std::vector<PreviewWindow*> windows;
    for (const auto& w : m_previews)
        if (w)
            windows.push_back(w);
    return windows;
}

PreviewWindow* MainWindow::openPreview(const VfsNode* node, bool properties)
{
    if (!node || !m_session)
        return nullptr;

    // One window per file: bring an open one to the front.
    for (PreviewWindow* w : previewWindows()) {
        if (w->node() == node) {
            if (properties)
                w->showNode(node, true);
            if (w->isMinimized())
                w->showNormal();
            w->raise();
            w->activateWindow();
            return w;
        }
    }

    // A top-level window of its own (own taskbar button, can go on another monitor).
    auto* w = new PreviewWindow();
    std::erase_if(m_previews, [](const QPointer<PreviewWindow>& p) { return p.isNull(); });
    if (!m_previews.empty()) // cascade from the newest window
        w->move(m_previews.back()->pos() + QPoint(32, 32));
    m_previews.emplace_back(w);
    if (m_offscreen)
        w->setAttribute(Qt::WA_DontShowOnScreen);

    w->setSession(m_session);
    w->setImportCheck(
        [this](const VfsNode* n) { return m_session && n && importKind(*m_session, *n) != ImportKind::None; });
    w->setEditMode(m_editMode);
    w->showNode(node, properties);
    w->setLocation(locationOf(node));
    w->setDropCheck([this, w](const QString& file) { return replaceRefusal(w->node(), file); });
    connect(w, &PreviewWindow::replaceRequested, this, [this, w](const QString& file) {
        if (file.isEmpty())
            replaceWithDialog(w->node(), w);
        else
            replaceNode(w->node(), file, w);
    });
    connect(w, &PreviewWindow::extractRequested, this, [this, w] {
        if (const VfsNode* n = w->node())
            extractNodes({n});
    });
    connect(w, &PreviewWindow::revealRequested, this, [this, w] {
        if (selectPath(w->location(), /*openFile=*/false)) {
            raise();
            activateWindow();
        }
    });
    connect(w, &PreviewWindow::backupRequested, this, [this, w] { backupNode(w->node()); });
    connect(w, &PreviewWindow::editRequested, this, [this, w] { openTextEditor(w->node()); });
    connect(w, &PreviewWindow::exportRequested, this, [this, w] {
        if (const VfsNode* n = w->node())
            exportNodes({n}, w);
    });
    connect(w, &PreviewWindow::importRequested, this, [this, w] { importNode(w->node(), w); });
    connect(w, &PreviewWindow::replaceSoundRequested, this, [this, w](int index) { replaceSound(w, index); });
    connect(w, &PreviewWindow::replaceTextureRequested, this,
            [this, w](int index, const QString& file) { replaceTexture(w, index, file); });
    connect(w, &PreviewWindow::importTexturesRequested, this, [this, w](const QStringList& files) {
        importTextures(w->node(), w->textureBrowser() ? static_cast<QWidget*>(w->textureBrowser()) : w, files);
    });

    updateBackupBanner();
    w->show();
    emit previewOpened(w);
    return w;
}

// ---- Text editor ------------------------------------------------------------------------------

std::vector<TextEditorWindow*> MainWindow::editorWindows() const
{
    std::vector<TextEditorWindow*> windows;
    for (const auto& w : m_editors)
        if (w)
            windows.push_back(w);
    return windows;
}

bool MainWindow::isEditableText(const VfsNode* node, bool sniff) const
{
    if (!m_session || !node || !node->isFile())
        return false;
    const FileCategory category = m_session->content.fileTypes().lookup(node->name()).category;
    if (category == FileCategory::Text)
        return true;
    if ((category != FileCategory::Unknown && category != FileCategory::Data) || node->size() > (16u << 20))
        return false;
    // Binary data a decompiler can compile back (game tables) is edited as its text.
    const auto head = m_session->vfs.read(*node, 4096);
    if (!head || head->empty())
        return false;
    if (const TextDecompiler* d = m_session->content.findDecompiler(extensionOf(node->name()), *head))
        return d->canRecompile();
    // Unknown types may still be text (configs with odd extensions): peek at the start.
    return sniff && looksLikeText(*head);
}

void MainWindow::openTextEditor(const VfsNode* node)
{
    if (!node || !m_session)
        return;
    const QString location = locationOf(node);
    for (TextEditorWindow* w : editorWindows()) {
        if (w->location() == location) {
            if (w->isMinimized())
                w->showNormal();
            w->raise();
            w->activateWindow();
            return;
        }
    }

    auto* w = new TextEditorWindow();
    if (const QString error = w->open(m_session, node, location); !error.isEmpty()) {
        delete w;
        QMessageBox::information(QApplication::activeWindow(), tr("Edit Text"), error);
        return;
    }
    std::erase_if(m_editors, [](const QPointer<TextEditorWindow>& p) { return p.isNull(); });
    m_editors.emplace_back(w);
    if (m_offscreen)
        w->setAttribute(Qt::WA_DontShowOnScreen);
    connect(w, &TextEditorWindow::saveRequested, this,
            [this, w](const QByteArray& bytes) { saveEditedText(w, bytes); });
    connect(w, &TextEditorWindow::revealRequested, this, [this, w] {
        if (selectPath(w->location(), /*openFile=*/false)) {
            raise();
            activateWindow();
        }
    });
    w->show();
    emit editorOpened(w);
}

void MainWindow::saveEditedText(TextEditorWindow* editor, const QByteArray& bytes)
{
    const VfsNode* node = editor->node();
    if (!m_session || !node || m_editWatcher->isRunning() || m_loadWatcher->isRunning()) {
        QMessageBox::information(editor, tr("Save"), tr("26Viewer is busy — try saving again in a moment."));
        editor->saveFinished(false);
        return;
    }
    if (!m_editMode) {
        // Saving writes into the game's files, which only Edit Mode allows.
        setEditMode(true);
        if (!m_editMode) {
            editor->saveFinished(false);
            return;
        }
    }
    ByteBuffer contents(static_cast<std::size_t>(bytes.size()));
    std::memcpy(contents.data(), bytes.constData(), contents.size());
    auto plan = planReplaceData(*m_session, *node, std::move(contents), tr("your edited text"));
    if (!plan) {
        QMessageBox::warning(editor, tr("Save"), toQString(plan.error().message));
        editor->saveFinished(false);
        return;
    }
    if (!confirmAndReplace(std::move(*plan), /*edited=*/true, editor)) {
        editor->saveFinished(false);
        return;
    }
    m_savingEditor = editor;
}

void MainWindow::goBack()
{
    if (const VfsNode* folder = m_folderView->folder(); folder && !m_model->isRoot(folder))
        navigateTo(folder->parent(), folder);
}

// ---- Remembering the last session -------------------------------------------------------------
// Stored in the settings INI (%APPDATA%/26Viewer/26Viewer.ini) under [session].

void MainWindow::rememberSession() const
{
    if (!m_session || !m_rememberSession)
        return;
    QSettings settings;
    std::error_code ec;
    const bool folder = std::filesystem::is_directory(m_session->source, ec);
    settings.setValue("session/kind", m_session->game ? "game" : folder ? "folder" : "archive");
    settings.setValue("session/game", m_session->game ? toQString(m_session->game->info().id) : QString());
    settings.setValue("session/path", toQString(m_session->source));
    QStringList extras;
    for (const auto& path : m_session->extraSources())
        extras << toQString(path);
    settings.setValue(extrasKey(m_session->game), extras);
}

bool MainWindow::reopenLastSession()
{
    QSettings settings;
    if (!settings.value("session/reopenOnStartup", true).toBool())
        return false;
    const QString kind = settings.value("session/kind").toString();
    const QString path = settings.value("session/path").toString();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;

    const Game* game = nullptr;
    if (kind == "game") {
        game = m_games.find(settings.value("session/game").toString().toStdString());
        if (!game)
            return false;
    } else if (kind != "archive" && kind != "folder") {
        return false;
    }
    std::vector<std::filesystem::path> extras;
    for (const QString& extra : settings.value(extrasKey(game)).toStringList())
        extras.push_back(toPath(extra));
    openSource(game, toPath(path), std::move(extras), settings.value("session/selection").toString());
    return true;
}

void MainWindow::reloadSession(const QString& location)
{
    if (m_session)
        openSource(m_session->game, m_session->source, m_session->extraSources(), location);
}

// ---- Edit Mode & backups ----------------------------------------------------------------------

void MainWindow::setEditMode(bool on, bool confirm)
{
    if (on && !m_editMode && confirm) {
        QMessageBox box(QMessageBox::Warning, tr("Enable Edit Mode?"),
                        tr("<b>Edit Mode lets 26Viewer change your game files on disk.</b><br><br>"
                           "Replacing a file rewrites the archive it lives in. A broken or incompatible "
                           "file can stop the game from loading, and without a backup the change can't "
                           "be undone.<br><br>"
                           "Archives without a backup are marked with a red banner — use "
                           "<b>Back Up Now</b> before editing them."),
                        QMessageBox::Cancel, this);
        QPushButton* enable = box.addButton(tr("Enable Edit Mode"), QMessageBox::AcceptRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        on = box.clickedButton() == enable;
    }
    m_editMode = on;
    m_editModeAction->setChecked(on); // keeps the toggle in sync if the user cancelled
    m_editModeBadge->setVisible(on);
    if (isVisible()) // the status bar can't overlay messages before the window is shown
        statusBar()->showMessage(on ? tr("Edit Mode on — game files can be modified")
                                    : tr("Edit Mode off — game files are read-only"),
                                 5000);
    for (PreviewWindow* w : previewWindows())
        w->setEditMode(on);
    updateBackupBanner();
    updateActions();
}

void MainWindow::updateBackupBanner()
{
    // Only while editing is possible, and only for things that can actually be backed up.
    const auto warningFor = [this](const VfsNode* node) {
        const auto target = node && m_session ? backupTarget(*node) : std::filesystem::path{};
        return m_editMode && !target.empty() && !hasBackup(*m_session, *node) ? toQString(target.filename()) : QString();
    };
    m_folderView->banner()->setWarning(warningFor(m_folderView->folder()));
    for (PreviewWindow* w : previewWindows())
        w->setBackupWarning(warningFor(w->node()));
}

void MainWindow::backupNode(const VfsNode* node)
{
    if (!node || !m_session || m_editWatcher->isRunning())
        return;
    const auto target = backupTarget(*node);
    if (target.empty() || hasBackup(*m_session, *node))
        return;
    m_folderView->banner()->setInProgress(true);
    for (PreviewWindow* w : previewWindows())
        w->setBackupInProgress(true);
    m_statusLabel->setText(tr("Backing up %1…").arg(toQString(target.filename())));
    m_progress->setRange(0, 0);
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_editWatcher->setFuture(startBackup(target, m_session->vfs.backupRoot()));
    updateActions();
}

// ---- Replacing assets -------------------------------------------------------------------------

void MainWindow::replaceSelected()
{
    if (const auto nodes = selectedNodes(); nodes.size() == 1)
        replaceWithDialog(nodes.front(), this);
}

void MainWindow::replaceWithDialog(const VfsNode* node, QWidget* dialogParent)
{
    if (!node || !m_editMode || !Vfs::canReplace(*node))
        return;
    const QString ext = toQString(extensionOf(node->name()));
    QSettings settings;
    const QString file = QFileDialog::getOpenFileName(
        dialogParent, tr("Replace %1 with…").arg(toQString(node->name())), settings.value("paths/lastReplaceDir").toString(),
        tr("%1 files (*.%2)").arg(ext.toUpper(), ext));
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastReplaceDir", QFileInfo(file).absolutePath());
    replaceNode(node, file, dialogParent);
}

void MainWindow::replaceNode(const VfsNode* node, const QString& file, QWidget* dialogParent)
{
    if (!m_session || !node || !m_editMode || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    QWidget* parent = dialogParent ? dialogParent : this; // dialogs appear over the window that asked

    auto plan = planReplace(*m_session, *node, file);
    if (!plan) {
        QMessageBox::warning(parent, tr("Replace"), toQString(plan.error().message));
        return;
    }
    confirmAndReplace(std::move(*plan), /*edited=*/false, parent);
}

bool MainWindow::confirmAndReplace(ReplacePlan plan, bool edited, QWidget* dialogParent)
{
    const VfsNode* node = plan.node;
    QWidget* parent = dialogParent ? dialogParent : this;
    const QString title = edited ? tr("Save") : tr("Replace");
    const QLocale locale;
    const QString where = node->archive() ? tr("inside <b>%1</b>").arg(toQString(node->archive()->path().filename()))
                                          : tr("on disk");
    const QString name = toQString(node->name()).toHtmlEscaped();
    QString text = edited ? tr("Save your changes to <b>%1</b> %2?").arg(name, where)
                          : tr("Replace <b>%1</b> %2 with <b>%3</b>?")
                                .arg(name, where, QFileInfo(plan.sourceFile).fileName().toHtmlEscaped());
    text += tr("<br><br>Size: %1 → %2")
                .arg(locale.formattedDataSize(static_cast<qint64>(node->size())),
                     locale.formattedDataSize(static_cast<qint64>(plan.data.size())));
    if (!plan.notes.isEmpty()) {
        text += QStringLiteral("<br>");
        for (const QString& note : plan.notes)
            text += QStringLiteral("<br>• ") + note.toHtmlEscaped();
    }
    if (!plan.warning.isEmpty())
        text += QStringLiteral("<br><br>⚠ ") + plan.warning.toHtmlEscaped();

    // No automatic backups: if there isn't one, say so and offer to make it.
    const auto target = Vfs::editTarget(*node);
    const auto backup = backupOf(*m_session, *node);
    const bool backedUp = !backup.empty();
    QMessageBox box(QMessageBox::Question, title, QString(), QMessageBox::Cancel, parent);
    QPushButton* replace = nullptr;
    QPushButton* backupAndReplace = nullptr;
    if (backedUp) {
        text += tr("<br><br>The original is backed up in %1.").arg(backupDisplayName(*m_session, backup).toHtmlEscaped());
        replace = box.addButton(title, QMessageBox::AcceptRole);
    } else {
        box.setIcon(QMessageBox::Warning);
        text += tr("<br><br><b style='color:#C42B1C'>%1 is not backed up — this change can't be undone.</b>")
                    .arg(toQString(target.filename()).toHtmlEscaped());
        backupAndReplace = box.addButton(edited ? tr("Back Up && Save") : tr("Back Up && Replace"),
                                         QMessageBox::AcceptRole);
        replace = box.addButton(edited ? tr("Save Without Backup") : tr("Replace Without Backup"),
                                QMessageBox::DestructiveRole);
    }
    box.setText(text);
    if (!plan.warning.isEmpty())
        box.setDefaultButton(QMessageBox::Cancel);
    else
        box.setDefaultButton(backupAndReplace ? backupAndReplace : replace);
    box.exec();
    if (box.clickedButton() == backupAndReplace && backupAndReplace)
        plan.backupFirst = true;
    else if (box.clickedButton() != replace)
        return false;

    // After the reload: select it again (open preview windows follow on their own).
    m_pendingSelect = locationOf(node);
    m_statusLabel->setText(edited ? tr("Saving %1…").arg(toQString(node->name()))
                                  : tr("Replacing %1…").arg(toQString(node->name())));
    m_progress->setRange(0, 0);
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_editWatcher->setFuture(startReplace(m_session, std::move(plan)));
    updateActions();
    return true;
}

void MainWindow::restoreOriginal()
{
    const auto nodes = selectedNodes();
    if (!m_session || !m_editMode || nodes.size() != 1 || !hasBackup(*m_session, *nodes.front())
        || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    const VfsNode* node = nodes.front();
    const auto target = backupTarget(*node);
    const QString backup = backupDisplayName(*m_session, backupOf(*m_session, *node));

    // One file inside an archive: put back just that file, as it is in the
    // archive's backup; the archive's other changes stay.
    if (restoresSingleFile(*node)) {
        if (QMessageBox::question(this, tr("Restore Original"),
                                  tr("Put back the original %1, as it is in the backup %2?\n\nOnly this file "
                                     "changes; other files replaced in %3 keep their changes.")
                                      .arg(toQString(node->name()), backup, toQString(target.filename())))
            != QMessageBox::Yes)
            return;
        m_pendingSelect = locationOf(node);
        m_statusLabel->setText(tr("Restoring %1…").arg(toQString(node->name())));
        m_progress->setRange(0, 0);
        m_progress->show();
        QApplication::setOverrideCursor(Qt::BusyCursor);
        m_editWatcher->setFuture(startRestoreFile(m_session, node));
        updateActions();
        return;
    }

    const QString name = toQString(target.filename());
    if (QMessageBox::question(this, tr("Restore Original"),
                              tr("Restore the original %1 from %2?\n\nEvery replacement made in it since the backup "
                                 "will be undone.")
                                  .arg(name, backup))
        != QMessageBox::Yes)
        return;

    // Close the session first so no open handle blocks overwriting the file;
    // remember what to reopen once the restore finishes.
    m_reopen = {m_session->game, m_session->source, m_session->extraSources()};
    const std::filesystem::path backupRoot = m_session->vfs.backupRoot();
    const QString reselect = locationOf(node);
    setSession(nullptr);
    m_pendingSelect = reselect;

    m_statusLabel->setText(tr("Restoring %1…").arg(name));
    m_progress->setRange(0, 0);
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_editWatcher->setFuture(startRestore(target, backupRoot));
    updateActions();
}

void MainWindow::onEditFinished()
{
    QApplication::restoreOverrideCursor();
    m_progress->hide();
    const EditResult result = m_editWatcher->result();

    if (result.kind == EditResult::Kind::Backup) { // nothing changed in the archive: no reload
        m_folderView->banner()->setInProgress(false);
        for (PreviewWindow* w : previewWindows())
            w->setBackupInProgress(false);
        if (!result.error.isEmpty())
            QMessageBox::warning(this, tr("Back Up"), result.error);
        else
            statusBar()->showMessage(result.summary, 15000);
        updateBackupBanner();
        updateActions();
        return;
    }

    if (!result.error.isEmpty()) {
        QMessageBox::warning(m_savingEditor ? static_cast<QWidget*>(m_savingEditor) : this, appName(), result.error);
        m_statusLabel->setText(tr("Edit failed"));
    }
    if (TextEditorWindow* editor = std::exchange(m_savingEditor, nullptr))
        editor->saveFinished(result.error.isEmpty());
    if (result.unchanged) { // nothing was written: no reload
        statusBar()->showMessage(result.summary, 15000);
        m_pendingSelect.clear();
        updateActions();
        return;
    }
    if (m_reopen) { // a restore closed the session: open it again
        const auto [game, source, extras] = *std::exchange(m_reopen, std::nullopt);
        openSource(game, source, extras, std::exchange(m_pendingSelect, QString()));
    } else if (result.error.isEmpty()) {
        reloadSession(m_pendingSelect); // show the new contents
    }
    if (result.error.isEmpty()) {
        statusBar()->showMessage(result.summary, 15000);
        if (!result.warnings.isEmpty())
            QMessageBox::information(this, tr("Replaced — please note"), result.warnings.join(QStringLiteral("\n\n")));
    }
    updateActions();
}

const VfsNode* MainWindow::nodeAt(const QAbstractItemView* view, const QPoint& pos) const
{
    if (view == m_tree)
        return VfsModel::nodeFromIndex(m_treeFilter->mapToSource(m_tree->indexAt(pos)));
    return m_folderView->nodeAt(pos);
}

const VfsNode* MainWindow::dropTarget(const QAbstractItemView* view, const QPoint& pos, const QMimeData* mime,
                                      QString* reason) const
{
    if (!m_session || !mime || !mime->hasUrls() || mime->urls().size() != 1)
        return nullptr;
    const QString file = mime->urls().front().toLocalFile();
    if (!QFileInfo(file).isFile())
        return nullptr;
    const VfsNode* node = nodeAt(view, pos);
    if (!node || !node->isFile())
        return nullptr;
    *reason = replaceRefusal(node, file);
    return reason->isEmpty() ? node : nullptr;
}

QString MainWindow::replaceRefusal(const VfsNode* node, const QString& file) const
{
    if (!m_session || !node || !node->isFile())
        return tr("Nothing to replace");
    if (!m_editMode)
        return tr("Turn on Edit Mode (Ctrl+M) to replace files");
    if (!Vfs::canReplace(*node))
        return tr("%1 can't be replaced").arg(toQString(node->name()));
    const QString targetExt = toQString(extensionOf(node->name()));
    if (QFileInfo(file).suffix().compare(targetExt, Qt::CaseInsensitive) != 0)
        return tr("Can't drop a .%1 file onto %2 (needs .%3)")
            .arg(QFileInfo(file).suffix(), toQString(node->name()), targetExt);
    return {};
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    QTreeView* view = watched == m_folderView->view()->viewport() ? m_folderView->view()
                      : watched == m_tree->viewport()             ? m_tree
                                                                  : nullptr;
    if (!view)
        return QMainWindow::eventFilter(watched, event);

    // Over the list or tree, a single dropped file either replaces the item under the
    // cursor (matching type), is refused with a reason (other type), or —
    // over empty space / folders — opens like a drop anywhere else.
    const auto isFileDrag = [](const QMimeData* mime) {
        return mime && mime->hasUrls() && mime->urls().size() == 1 && mime->urls().front().isLocalFile();
    };

    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (!isFileDrag(drag->mimeData()))
            return false;
        QString reason;
        const QPoint pos = drag->position().toPoint();
        if (const VfsNode* node = dropTarget(view, pos, drag->mimeData(), &reason)) {
            drag->acceptProposedAction();
            // Highlight the row that would be replaced (selection only, no navigation).
            view->selectionModel()->select(view->indexAt(pos),
                                           QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            statusBar()->showMessage(tr("Drop to replace %1").arg(toQString(node->name())));
        } else if (!reason.isEmpty()) {
            drag->setDropAction(Qt::IgnoreAction); // "not allowed" cursor
            drag->accept();
            statusBar()->showMessage(reason);
        } else {
            drag->acceptProposedAction();
            statusBar()->showMessage(tr("Drop to open %1").arg(drag->mimeData()->urls().front().fileName()));
        }
        return true;
    }
    case QEvent::DragLeave:
        statusBar()->clearMessage();
        return false;
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        if (!isFileDrag(drop->mimeData()))
            return false;
        statusBar()->clearMessage();
        QString reason;
        const QString file = drop->mimeData()->urls().front().toLocalFile();
        const VfsNode* node = dropTarget(view, drop->position().toPoint(), drop->mimeData(), &reason);
        drop->acceptProposedAction();
        // Act after the drop completes so Explorer isn't blocked by our dialogs.
        if (node)
            QTimer::singleShot(0, this, [this, node, file] { replaceNode(node, file); });
        else if (reason.isEmpty())
            QTimer::singleShot(0, this, [this, file] { openPath(file); });
        else
            QTimer::singleShot(0, this, [this, reason] { QMessageBox::information(this, tr("Replace"), reason); });
        return true;
    }
    default:
        return QMainWindow::eventFilter(watched, event);
    }
}

// ---- Selection & actions ---------------------------------------------------------------------

std::vector<const VfsNode*> MainWindow::selectedNodes() const
{
    if (!m_session)
        return {};
    // What's selected in the list (unless the tree has focus); else the
    // folder selected in the tree.
    if (!m_tree->hasFocus())
        if (auto nodes = m_folderView->selectedNodes(); !nodes.empty())
            return nodes;
    if (const VfsNode* folder = treeNode())
        return {folder};
    return {};
}

void MainWindow::updateActions()
{
    const bool loading = m_loadWatcher->isRunning();
    const bool extracting = m_extractWatcher->isRunning();
    const bool editing = m_editWatcher->isRunning();
    const bool busy = loading || extracting || editing;
    const bool hasSession = static_cast<bool>(m_session);
    const auto nodes = hasSession ? selectedNodes() : std::vector<const VfsNode*>{};
    const VfsNode* single = nodes.size() == 1 ? nodes.front() : nullptr;

    const bool opening = busy || m_mountWatcher->isRunning();
    m_openGameAction->setEnabled(!opening);
    m_openArchiveAction->setEnabled(!opening);
    m_openFolderAction->setEnabled(!opening);
    m_extractAction->setEnabled(hasSession && !nodes.empty() && !extracting);
    m_extractAllAction->setEnabled(hasSession && !extracting);
    m_exportAction->setEnabled(hasSession && !nodes.empty() && !extracting);
    // Only looked up in Edit Mode: it reads the start of the file.
    m_importAction->setEnabled(m_editMode && !busy && single && importKind(*m_session, *single) != ImportKind::None);
    m_editModeAction->setEnabled(!editing);
    m_replaceAction->setEnabled(m_editMode && !busy && single && Vfs::canReplace(*single));
    m_restoreAction->setEnabled(m_editMode && !busy && single && hasBackup(*m_session, *single));
    m_editTextAction->setEnabled(isEditableText(single, /*sniff=*/false));
    m_copyPathAction->setEnabled(!nodes.empty());
    m_revealAction->setEnabled(single && !single->diskPath().empty());
    m_openAction->setEnabled(single != nullptr);
    m_propertiesAction->setEnabled(single != nullptr);
    const VfsNode* folder = m_folderView->folder();
    m_backAction->setEnabled(folder && !m_model->isRoot(folder));
}

void MainWindow::showTreeContextMenu(const QPoint& pos)
{
    if (!m_tree->indexAt(pos).isValid())
        return;
    m_tree->setFocus(); // the menu acts on the tree's folder, not the list selection
    updateActions();
    QMenu menu(this);
    menu.addAction(m_extractAction);
    menu.addAction(m_exportAction);
    if (m_restoreAction->isEnabled())
        menu.addAction(m_restoreAction);
    menu.addSeparator();
    menu.addAction(m_copyPathAction);
    menu.addAction(m_revealAction);
    menu.addSeparator();
    menu.addAction(m_propertiesAction);
    // Archives and folders opened alongside the game can be closed again.
    const VfsNode* node = treeNode();
    if (m_session && node && m_model->isRoot(node) && node != &m_session->vfs.root()) {
        menu.addSeparator();
        QAction* close = menu.addAction(icons::theme(QIcon::ThemeIcon::WindowClose, QStyle::SP_DialogCloseButton),
                                        tr("&Close %1").arg(m_model->data(m_model->indexForNode(node)).toString()));
        connect(close, &QAction::triggered, this, [this, node] { closeMount(node); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MainWindow::showListContextMenu(const QPoint& globalPos)
{
    QTreeView* list = m_folderView->view();
    if (!list->indexAt(list->viewport()->mapFromGlobal(globalPos)).isValid())
        m_folderView->clearSelection(); // empty space: act on the open folder
    updateActions();
    QMenu menu(this);
    const auto nodes = m_folderView->selectedNodes();
    if (nodes.size() == 1) {
        m_openAction->setText(nodes.front()->isFile() ? tr("&Preview") : tr("&Open"));
        menu.addAction(m_openAction);
        menu.setDefaultAction(m_openAction);
        menu.addSeparator();
    }
    if (nodes.size() == 1 && isEditableText(nodes.front(), /*sniff=*/true)) {
        m_editTextAction->setEnabled(true);
        menu.addAction(m_editTextAction);
    }
    menu.addAction(m_extractAction);
    menu.addAction(m_exportAction);
    menu.addSeparator();
    menu.addAction(m_replaceAction);
    if (m_importAction->isEnabled())
        menu.addAction(m_importAction);
    if (m_restoreAction->isEnabled())
        menu.addAction(m_restoreAction);
    menu.addSeparator();
    menu.addAction(m_copyPathAction);
    menu.addAction(m_revealAction);
    menu.addSeparator();
    menu.addAction(m_propertiesAction);
    menu.exec(globalPos);
}

// ---- Extraction ------------------------------------------------------------------------------

void MainWindow::extract(bool everything)
{
    if (!m_session)
        return;
    if (everything)
        extractNodes({&m_session->vfs.root()});
    else
        extractNodes(selectedNodes());
}

void MainWindow::extractNodes(const std::vector<const VfsNode*>& nodes)
{
    if (!m_session || m_extractWatcher->isRunning())
        return;
    std::vector<ExtractItem> items;
    if (nodes.size() == 1 && nodes.front() == &m_session->vfs.root()) { // everything, laid out as in the game
        for (const VfsNode* file : Vfs::collectFiles(m_session->vfs.root()))
            items.push_back({file, file->virtualPath()});
    } else {
        items = planExtraction(nodes);
    }
    if (items.empty())
        return;

    QSettings settings;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Extract %n file(s) to", nullptr, static_cast<int>(items.size())),
        settings.value("paths/lastExtractDir").toString());
    if (dir.isEmpty())
        return;
    settings.setValue("paths/lastExtractDir", dir);

    m_extractTarget = dir;
    m_exporting = false;
    m_progress->setRange(0, static_cast<int>(items.size()));
    m_progress->setValue(0);
    m_progress->show();
    m_cancel->show();
    m_extractWatcher->setFuture(startExtraction(m_session, std::move(items), toPath(dir)));
    updateActions();
}

void MainWindow::onExtractFinished()
{
    m_progress->hide();
    m_cancel->hide();
    updateActions();
    if (m_extractWatcher->future().resultCount() == 0) {
        m_statusLabel->setText(m_exporting ? tr("Export cancelled") : tr("Extraction cancelled"));
        return;
    }
    const ExtractSummary summary = m_extractWatcher->result();
    const QString link = QUrl::fromLocalFile(m_extractTarget).toString();
    QString message;
    if (m_exporting) {
        message = summary.canceled ? tr("Export cancelled after %n file(s)", nullptr, summary.written)
                                   : tr("Exported %n file(s)", nullptr, summary.written);
        message += QStringLiteral(" (%1)").arg(tr("%n converted", nullptr, summary.converted));
    } else {
        message = summary.canceled ? tr("Extraction cancelled after %n file(s)", nullptr, summary.written)
                                   : tr("Extracted %n file(s)", nullptr, summary.written);
        message += QStringLiteral(" (%1)").arg(sizeText(summary.bytes));
    }
    message += QStringLiteral(" — <a href=\"%1\">%2</a>").arg(link, tr("Open folder"));
    m_statusLabel->setText(message);

    if (summary.failed > 0) {
        QMessageBox box(QMessageBox::Warning, appName(),
                        m_exporting ? tr("%n file(s) could not be exported.", nullptr, summary.failed)
                                    : tr("%n file(s) could not be extracted.", nullptr, summary.failed),
                        QMessageBox::Ok, this);
        box.setDetailedText(summary.errors.join('\n'));
        box.exec();
    }
}

// ---- Export / Import --------------------------------------------------------------------------

void MainWindow::exportNodes(const std::vector<const VfsNode*>& nodes, QWidget* dialogParent)
{
    if (!m_session || m_extractWatcher->isRunning())
        return;
    QWidget* parent = dialogParent ? dialogParent : this;
    std::vector<ExtractItem> items;
    if (nodes.size() == 1 && nodes.front() == &m_session->vfs.root()) {
        for (const VfsNode* file : Vfs::collectFiles(m_session->vfs.root()))
            items.push_back({file, file->virtualPath()});
    } else {
        items = planExtraction(nodes);
    }
    if (items.empty())
        return;

    ExportOptions options;
    if (hasTextures(*m_session, items)) {
        const auto images = chooseImageExport(parent);
        if (!images)
            return;
        options.images = *images;
    }
    QSettings settings;
    const QString dir = QFileDialog::getExistingDirectory(
        parent, tr("Export %n file(s) to", nullptr, static_cast<int>(items.size())),
        settings.value("paths/lastExtractDir").toString());
    if (dir.isEmpty())
        return;
    settings.setValue("paths/lastExtractDir", dir);

    m_extractTarget = dir;
    m_exporting = true;
    m_progress->setRange(0, static_cast<int>(items.size()));
    m_progress->setValue(0);
    m_progress->show();
    m_cancel->show();
    m_extractWatcher->setFuture(startExport(m_session, std::move(items), toPath(dir), options));
    updateActions();
}

bool MainWindow::ensureEditMode()
{
    if (!m_editMode)
        setEditMode(true); // asks first
    return m_editMode;
}

void MainWindow::importNode(const VfsNode* node, QWidget* dialogParent)
{
    if (!m_session || !node || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    QWidget* parent = dialogParent ? dialogParent : this;
    const ImportKind kind = importKind(*m_session, *node);
    const QString name = toQString(node->name());
    if (kind == ImportKind::None) {
        QMessageBox::information(parent, tr("Import"),
                                 tr("Nothing can be imported into %1 yet — only sound banks and textures.").arg(name));
        return;
    }
    if (kind == ImportKind::TextureDictionary) {
        importTextures(node, parent, {});
        return;
    }
    if (!ensureEditMode())
        return;

    QSettings settings;
    const QString lastDir = settings.value("paths/lastImportDir", settings.value("paths/lastExtractDir")).toString();
    auto session = m_session; // kept alive while the worker runs

    if (kind == ImportKind::SoundBank) {
        const QString dir = QFileDialog::getExistingDirectory(
            parent, tr("Import into %1 the .wav files from").arg(name), lastDir);
        if (dir.isEmpty())
            return;
        settings.setValue("paths/lastImportDir", dir);
        auto result = runBusy<Result<BankImport>>(parent, tr("Encoding sounds for %1…").arg(name),
                                                  [session, node, folder = toPath(dir)] {
                                                      return planBankImport(*session, *node, folder);
                                                  });
        if (!result) {
            QMessageBox::warning(parent, tr("Import"), toQString(result.error().message));
            return;
        }
        auto plan = planReplaceData(*session, *node, std::move(result->data),
                                    tr("the .wav files in %1").arg(QFileInfo(dir).fileName()));
        if (!plan) {
            QMessageBox::warning(parent, tr("Import"), toQString(plan.error().message));
            return;
        }
        plan->notes << tr("%n sound(s) replaced", nullptr, result->replaced)
                    << tr("%n sound(s) with no matching file stay as they were", nullptr, result->kept);
        QStringList problems;
        if (!result->unmatched.isEmpty())
            problems << tr("%n file(s) match no sound and are skipped: %1", nullptr,
                           static_cast<int>(result->unmatched.size()))
                            .arg(result->unmatched.mid(0, 5).join(QStringLiteral(", ")));
        if (!result->failed.isEmpty())
            problems << tr("%n file(s) couldn't be used: %1", nullptr, static_cast<int>(result->failed.size()))
                            .arg(result->failed.mid(0, 3).join(QStringLiteral("; ")));
        plan->warning = problems.join(QStringLiteral(" "));
        confirmAndReplace(std::move(*plan), /*edited=*/false, parent);
        return;
    }

    // Texture: a picture, converted to the pixel format the user picks.
    const QString file = QFileDialog::getOpenFileName(
        parent, tr("Import a picture into %1").arg(name), lastDir,
        tr("Pictures (*.png *.dds *.jpg *.jpeg *.bmp);;All files (*)"));
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastImportDir", QFileInfo(file).absolutePath());
    auto target = textureTarget(*session, *node);
    if (!target) {
        QMessageBox::warning(parent, tr("Import"), toQString(target.error().message));
        return;
    }
    auto picture = loadPicture(toPath(file));
    if (!picture) {
        QMessageBox::warning(parent, tr("Import"),
                             tr("Couldn't read %1: %2").arg(QFileInfo(file).fileName(), toQString(picture.error().message)));
        return;
    }
    const auto format = chooseTextureFormat(parent, *target, *picture, QFileInfo(file).fileName(), name);
    if (!format)
        return;
    struct Built {
        Result<ByteBuffer> data;
        QStringList notes;
    };
    const Built built = runBusy<Built>(parent, tr("Converting %1…").arg(QFileInfo(file).fileName()),
                                       [session, node, picture = std::move(*picture), format = *format] {
                                           Built b{ByteBuffer{}, {}};
                                           b.data = buildTextureImport(*session, *node, picture, format, b.notes);
                                           return b;
                                       });
    if (!built.data) {
        QMessageBox::warning(parent, tr("Import"), toQString(built.data.error().message));
        return;
    }
    auto plan = planReplaceData(*session, *node, *built.data,
                                tr("%1 as %2").arg(QFileInfo(file).fileName(), toQString(image::pixelFormatName(*format))));
    if (!plan) {
        QMessageBox::warning(parent, tr("Import"), toQString(plan.error().message));
        return;
    }
    plan->notes = built.notes;
    confirmAndReplace(std::move(*plan), /*edited=*/false, parent);
}

void MainWindow::replaceTexture(PreviewWindow* window, int index, QString file)
{
    const VfsNode* node = window ? window->node() : nullptr;
    if (!m_session || !node || index < 0 || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    QWidget* parent = window->textureBrowser() ? static_cast<QWidget*>(window->textureBrowser()) : window;
    if (!textureDictionary(*m_session, *node) || !ensureEditMode())
        return;
    if (file.isEmpty()) {
        QSettings settings;
        file = QFileDialog::getOpenFileName(
            parent, tr("Replace the texture with…"),
            settings.value("paths/lastImportDir", settings.value("paths/lastExtractDir")).toString(),
            tr("Pictures (*.png *.dds *.jpg *.jpeg *.bmp);;All files (*)"));
        if (file.isEmpty())
            return;
        settings.setValue("paths/lastImportDir", QFileInfo(file).absolutePath());
    }
    auto picture = loadPicture(toPath(file));
    if (!picture) {
        QMessageBox::warning(parent, tr("Replace Texture"),
                             tr("Couldn't read %1: %2").arg(QFileInfo(file).fileName(), toQString(picture.error().message)));
        return;
    }
    auto session = m_session;
    auto result = runBusy<Result<DictionaryImport>>(
        parent, tr("Converting %1…").arg(QFileInfo(file).fileName()),
        [session, node, index, picture = std::move(*picture)] {
            return planDictionaryReplace(*session, *node, static_cast<std::size_t>(index), picture);
        });
    confirmDictionaryImport(node, std::move(result), QFileInfo(file).fileName(), parent);
}

void MainWindow::importTextures(const VfsNode* node, QWidget* dialogParent, const QStringList& files)
{
    if (!m_session || !node || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    QWidget* parent = dialogParent ? dialogParent : this;
    if (!textureDictionary(*m_session, *node) || !ensureEditMode())
        return;
    std::vector<std::filesystem::path> pictures;
    QString source;
    if (files.isEmpty()) {
        QSettings settings;
        const QString dir = QFileDialog::getExistingDirectory(
            parent, tr("Import into %1 the pictures from").arg(toQString(node->name())),
            settings.value("paths/lastImportDir", settings.value("paths/lastExtractDir")).toString());
        if (dir.isEmpty())
            return;
        settings.setValue("paths/lastImportDir", dir);
        pictures = picturesIn(toPath(dir));
        source = tr("the pictures in %1").arg(QFileInfo(dir).fileName());
    } else {
        for (const QString& file : files)
            pictures.push_back(toPath(file));
        source = files.size() == 1 ? QFileInfo(files.front()).fileName()
                                   : tr("%n dropped picture(s)", nullptr, static_cast<int>(files.size()));
    }
    auto session = m_session;
    auto result = runBusy<Result<DictionaryImport>>(parent, tr("Converting pictures for %1…").arg(toQString(node->name())),
                                                    [session, node, pictures = std::move(pictures)] {
                                                        return planDictionaryImport(*session, *node, pictures);
                                                    });
    confirmDictionaryImport(node, std::move(result), source, parent);
}

void MainWindow::confirmDictionaryImport(const VfsNode* node, Result<DictionaryImport> result, const QString& source,
                                         QWidget* parent)
{
    if (!result) {
        QMessageBox::warning(parent, tr("Textures"), toQString(result.error().message));
        return;
    }
    auto plan = planReplaceData(*m_session, *node, std::move(result->data), source);
    if (!plan) {
        QMessageBox::warning(parent, tr("Textures"), toQString(plan.error().message));
        return;
    }
    plan->notes << tr("%n texture(s) replaced", nullptr, result->replaced)
                << tr("%n texture(s) stay as they were", nullptr, result->kept);
    plan->notes << result->notes.mid(0, 8);
    QStringList problems;
    if (!result->unmatched.isEmpty())
        problems << tr("%n picture(s) match no texture and are skipped: %1", nullptr,
                       static_cast<int>(result->unmatched.size()))
                        .arg(result->unmatched.mid(0, 5).join(QStringLiteral(", ")));
    if (!result->failed.isEmpty())
        problems << tr("%n picture(s) couldn't be used: %1", nullptr, static_cast<int>(result->failed.size()))
                        .arg(result->failed.mid(0, 3).join(QStringLiteral("; ")));
    plan->warning = problems.join(QStringLiteral(" "));
    confirmAndReplace(std::move(*plan), /*edited=*/false, parent);
}

void MainWindow::replaceSound(PreviewWindow* window, int index)
{
    const VfsNode* node = window ? window->node() : nullptr;
    if (!m_session || !node || index < 0 || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;
    if (importKind(*m_session, *node) != ImportKind::SoundBank || !ensureEditMode())
        return;
    QSettings settings;
    const QString file = QFileDialog::getOpenFileName(
        window, tr("Replace the sound with…"),
        settings.value("paths/lastImportDir", settings.value("paths/lastExtractDir")).toString(),
        tr("Wave audio (*.wav)"));
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastImportDir", QFileInfo(file).absolutePath());
    auto session = m_session;
    auto result = runBusy<Result<BankImport>>(window, tr("Encoding %1…").arg(QFileInfo(file).fileName()),
                                              [session, node, index, path = toPath(file)] {
                                                  return planSoundReplace(*session, *node,
                                                                          static_cast<std::size_t>(index), path);
                                              });
    if (!result) {
        QMessageBox::warning(window, tr("Replace Sound"), toQString(result.error().message));
        return;
    }
    auto plan = planReplaceData(*session, *node, std::move(result->data), QFileInfo(file).fileName());
    if (!plan) {
        QMessageBox::warning(window, tr("Replace Sound"), toQString(plan.error().message));
        return;
    }
    plan->notes << tr("Only sound #%1 changes; the other %n stay as they were.", nullptr, result->kept).arg(index + 1);
    confirmAndReplace(std::move(*plan), /*edited=*/false, window);
}

// ---- Window events ----------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event)
{
    for (TextEditorWindow* w : editorWindows()) {
        if (!w->close()) { // unsaved changes and the user cancelled (or a save is running)
            event->ignore();
            return;
        }
    }
    if (m_extractWatcher->isRunning()) {
        m_extractWatcher->cancel();
        m_extractWatcher->waitForFinished();
    }
    QSettings settings;
    // Reopened at next start-up (see reopenLastSession): the previewed file,
    // or the open folder.
    if (m_rememberSession) {
        const auto selected = m_folderView->selectedNodes();
        const VfsNode* current = selected.size() == 1 ? selected.front() : m_folderView->folder();
        settings.setValue("session/selection", locationOf(current));
        settings.remove("session/previewOpen");
    }
    for (PreviewWindow* w : previewWindows())
        w->close();
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("window/listHeader", m_folderView->saveHeaderState());
    if (auto* splitter = findChild<QSplitter*>("mainSplitter"))
        settings.setValue("window/browserSplitter", splitter->saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().value(0).isLocalFile())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty())
        openPath(urls.front().toLocalFile());
}

void MainWindow::showAbout()
{
    QStringList games;
    for (const auto& g : m_games.games())
        games << QStringLiteral("%1 (%2)").arg(toQString(g->info().title)).arg(g->info().year);
    QMessageBox::about(this, tr("About %1").arg(appName()),
                       tr("<h3>%1 %2</h3>"
                          "<p>A modular viewer for game archives.</p>"
                          "<p><b>Games in this build:</b> %3</p>"
                          "<p>Built with Qt %4 · style: %5</p>")
                           .arg(appName(), QApplication::applicationVersion(),
                                games.isEmpty() ? tr("none") : games.join(QStringLiteral(", ")), qVersion(),
                                QApplication::style()->name()));
}

} // namespace viewer::app
