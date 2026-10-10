#include "MainWindow.hpp"

#include "GamePickerDialog.hpp"
#include "Icons.hpp"
#include "QtConvert.hpp"
#include "Theme.hpp"
#include "VfsModel.hpp"
#include "preview/PreviewPane.hpp"

#include "viewer/core/RandomAccessFile.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
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

QString sizeText(std::uint64_t bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

MainWindow::MainWindow(const GameRegistry& games, ThemeManager& theme, QWidget* parent)
    : QMainWindow(parent), m_games(games), m_theme(theme), m_loadWatcher(new QFutureWatcher<LoadResult>(this)),
      m_extractWatcher(new QFutureWatcher<ExtractSummary>(this)),
      m_editWatcher(new QFutureWatcher<EditResult>(this)), m_model(new VfsModel(this)),
      m_filter(new VfsFilterModel(this)), m_leftStack(nullptr), m_tree(nullptr), m_placeholder(nullptr),
      m_preview(nullptr), m_search(nullptr), m_statusLabel(new QLabel(this)), m_progress(new QProgressBar(this)),
      m_cancel(new QToolButton(this))
{
    setWindowTitle(appName());
    setWindowIcon(icons::appIcon());
    setAcceptDrops(true);
    m_filter->setSourceModel(m_model);

    buildActions();
    buildUi();

    connect(m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &MainWindow::onLoadFinished);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::finished, this, &MainWindow::onExtractFinished);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::progressRangeChanged, m_progress,
            &QProgressBar::setRange);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::progressValueChanged, m_progress,
            &QProgressBar::setValue);
    connect(m_extractWatcher, &QFutureWatcher<ExtractSummary>::progressTextChanged, this,
            [this](const QString& text) { m_statusLabel->setText(tr("Extracting %1…").arg(text)); });
    connect(m_editWatcher, &QFutureWatcher<EditResult>::finished, this, &MainWindow::onEditFinished);

    QSettings settings;
    if (!restoreGeometry(settings.value("window/geometry").toByteArray()))
        resize(1280, 800);
    restoreState(settings.value("window/state").toByteArray());
    updateActions();
}

MainWindow::~MainWindow()
{
    m_loadWatcher->waitForFinished();
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
    m_openArchiveAction->setStatusTip(tr("Browse a single archive file"));
    connect(m_openArchiveAction, &QAction::triggered, this, &MainWindow::openArchiveDialog);

    m_extractAction =
        new QAction(icons::theme(TI::DocumentSaveAs, QStyle::SP_DialogSaveButton), tr("&Extract Selected…"), this);
    m_extractAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(m_extractAction, &QAction::triggered, this, [this] { extract(false); });

    m_extractAllAction = new QAction(icons::theme(TI::DocumentSave, QStyle::SP_DialogSaveButton), tr("Extract A&ll…"), this);
    m_extractAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(m_extractAllAction, &QAction::triggered, this, [this] { extract(true); });

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
    m_restoreAction->setStatusTip(tr("Put back the original archive / file from its .bak backup"));
    connect(m_restoreAction, &QAction::triggered, this, &MainWindow::restoreOriginal);

    m_editModeAction = new QAction(icons::editMode(), tr("Edit &Mode"), this);
    m_editModeAction->setCheckable(true);
    m_editModeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    m_editModeAction->setStatusTip(tr("Unlock replacing and restoring game files"));
    connect(m_editModeAction, &QAction::triggered, this, [this](bool on) { setEditMode(on); });

    m_reopenAction = new QAction(tr("Reopen Last Game at &Startup"), this);
    m_reopenAction->setCheckable(true);
    m_reopenAction->setChecked(QSettings().value("session/reopenOnStartup", true).toBool());
    connect(m_reopenAction, &QAction::toggled, this,
            [](bool on) { QSettings().setValue("session/reopenOnStartup", on); });

    // Menus
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_openGameAction);
    file->addAction(m_openArchiveAction);
    file->addSeparator();
    file->addAction(m_extractAction);
    file->addAction(m_extractAllAction);
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

    QMenu* view = menuBar()->addMenu(tr("&View"));
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
    toolbar->addSeparator();
    toolbar->addAction(m_extractAction);
    toolbar->addAction(m_extractAllAction);
    toolbar->addSeparator();
    toolbar->addAction(m_replaceAction);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Edit Mode stands apart on the right, next to search, so its state is always obvious.
    toolbar->addAction(m_editModeAction);
    auto* gap = new QWidget(toolbar);
    gap->setFixedWidth(12);
    toolbar->addWidget(gap);

    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Filter files  (Ctrl+F)"));
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
        const QString text = m_search->text().trimmed();
        m_filter->setFilterFixedString(text);
        if (text.isEmpty()) {
            m_tree->collapseAll();
            m_tree->expandToDepth(0);
        } else {
            m_tree->expandAll();
        }
    });

    // Tree
    m_tree = new QTreeView(this);
    m_tree->setModel(m_filter);
    m_tree->setUniformRowHeights(true);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(VfsModel::Name, Qt::AscendingOrder);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setAlternatingRowColors(false);
    // Files dragged from Explorer onto an item replace it (see eventFilter).
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->viewport()->installEventFilter(this);
    configureTreeHeader();
    m_tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree->header(), &QHeaderView::customContextMenuRequested, this, &MainWindow::showHeaderMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &MainWindow::showTreeContextMenu);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        m_preview->showNode(VfsModel::nodeFromIndex(m_filter->mapToSource(index)));
        updateBackupBanner();
    });
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);

    m_placeholder = buildPlaceholder();
    m_leftStack = new QStackedWidget(this);
    m_leftStack->addWidget(m_placeholder);
    m_leftStack->addWidget(m_tree);

    m_preview = new PreviewPane(this);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("mainSplitter");
    splitter->addWidget(m_leftStack);
    splitter->addWidget(m_preview);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    if (!splitter->restoreState(QSettings().value("window/splitter").toByteArray()))
        splitter->setSizes({640, 760});
    setCentralWidget(splitter);

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

    connect(m_preview, &PreviewPane::backupRequested, this, &MainWindow::backupCurrent);
}

void MainWindow::configureTreeHeader()
{
    // QHeaderView drops per-section resize modes on a model reset, so this
    // runs at startup and again whenever a session is attached.
    QHeaderView* header = m_tree->header();
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(60);
    const QByteArray saved = QSettings().value("window/treeHeader").toByteArray();
    if (saved.isEmpty() || !header->restoreState(saved)) {
        header->resizeSection(VfsModel::Type, 140);
        header->resizeSection(VfsModel::Size, 80);
        header->resizeSection(VfsModel::Packed, 80);
        header->resizeSection(VfsModel::Modified, 140);
        header->setSectionHidden(VfsModel::Modified, true); // available via the header menu
    }
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(VfsModel::Name, QHeaderView::Stretch);
}

void MainWindow::showHeaderMenu(const QPoint& pos)
{
    QHeaderView* header = m_tree->header();
    QMenu menu(this);
    for (int column = VfsModel::Type; column < VfsModel::ColumnCount; ++column) {
        QAction* action = menu.addAction(m_model->headerData(column, Qt::Horizontal).toString());
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(column));
        connect(action, &QAction::toggled, header,
                [header, column](bool visible) { header->setSectionHidden(column, !visible); });
    }
    menu.exec(header->viewport()->mapToGlobal(pos));
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
    if (info.isDir()) {
        if (const Game* game = m_games.detect(toPath(info.absoluteFilePath()))) {
            openGame(*game, toPath(info.absoluteFilePath()));
            return;
        }
        QMessageBox::warning(this, appName(),
                             tr("None of the games in this build recognise\n%1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (info.isFile()) {
        openArchive(toPath(info.absoluteFilePath()));
        return;
    }
    QMessageBox::warning(this, appName(), tr("%1 does not exist.").arg(QDir::toNativeSeparators(path)));
}

void MainWindow::openGame(const Game& game, const std::filesystem::path& directory)
{
    startLoad(tr("Loading %1…").arg(toQString(game.info().title)), [&game, directory] {
        return LoadResult{std::shared_ptr<Session>(openGameSession(game, directory)), {}};
    });
}

void MainWindow::openArchiveDialog()
{
    QSettings settings;
    QStringList patterns;
    for (const auto& g : m_games.games()) {
        ContentRegistry probe;
        g->registerContent(probe);
        for (const auto& format : probe.archiveFormats())
            for (const auto& ext : format->extensions())
                patterns << QStringLiteral("*.%1").arg(toQString(ext));
    }
    patterns.removeDuplicates();
    const QString filter = tr("Archives (%1);;All files (*)").arg(patterns.join(' '));
    const QString file = QFileDialog::getOpenFileName(this, tr("Open Archive"),
                                                      settings.value("paths/lastArchiveDir").toString(), filter);
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastArchiveDir", QFileInfo(file).absolutePath());
    openArchive(toPath(file));
}

void MainWindow::openArchive(const std::filesystem::path& file)
{
    const GameRegistry* games = &m_games;
    startLoad(tr("Opening %1…").arg(toQString(file.filename())), [games, file] {
        auto session = openArchiveSession(*games, file);
        if (!session)
            return LoadResult{nullptr, toQString(session.error().message)};
        return LoadResult{std::shared_ptr<Session>(std::move(*session)), {}};
    });
}

void MainWindow::startLoad(const QString& message, std::function<LoadResult()> job)
{
    if (m_loadWatcher->isRunning())
        return;
    m_statusLabel->setText(message);
    m_progress->setRange(0, 0); // indeterminate
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_loadWatcher->setFuture(QtConcurrent::run(std::move(job)));
    updateActions();
}

void MainWindow::onLoadFinished()
{
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
    const auto& report = result.session->report;
    if (!report.warnings.empty()) {
        QStringList lines;
        for (const auto& w : report.warnings)
            lines << toQString(w.message);
        QMessageBox::warning(this, appName(),
                             tr("Some archives could not be opened:\n\n%1").arg(lines.mid(0, 15).join('\n')));
    }
}

void MainWindow::setSession(std::shared_ptr<Session> session)
{
    m_preview->setSession(nullptr);
    m_model->setSource(nullptr, nullptr);
    m_session = std::move(session);
    if (m_session) {
        m_model->setSource(&m_session->vfs.root(), &m_session->content);
        configureTreeHeader();
        m_preview->setSession(m_session);
        m_leftStack->setCurrentWidget(m_tree);
        m_tree->expandToDepth(0);
        setWindowTitle(QStringLiteral("%1 — %2").arg(m_session->title(), appName()));
        const VfsNode& root = m_session->vfs.root();
        m_statusLabel->setText(tr("%1  ·  %n archive(s)", nullptr, static_cast<int>(m_session->report.archives))
                                   .arg(m_session->title())
                               + tr("  ·  %n file(s)", nullptr, static_cast<int>(root.fileCount()))
                               + QStringLiteral("  ·  %1").arg(sizeText(root.size())));
        rememberSession();
        if (!m_pendingSelect.isEmpty()) {
            selectPath(m_pendingSelect);
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

bool MainWindow::selectPath(const QString& virtualPath)
{
    if (!m_session)
        return false;
    const VfsNode* node = &m_session->vfs.root();
    for (const QString& part : virtualPath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const VfsNode* next = nullptr;
        for (const auto& child : node->children())
            if (toQString(child->name()).compare(part, Qt::CaseInsensitive) == 0)
                next = child.get();
        if (!next)
            return false;
        node = next;
    }
    const QModelIndex index = m_filter->mapFromSource(m_model->indexForNode(node));
    if (!index.isValid())
        return false;
    m_tree->setCurrentIndex(index);
    m_tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
    return true;
}

// ---- Remembering the last session -------------------------------------------------------------
// Stored in the settings INI (%APPDATA%/26Viewer/26Viewer.ini) under [session].

void MainWindow::rememberSession() const
{
    if (!m_session || !m_rememberSession)
        return;
    QSettings settings;
    settings.setValue("session/kind", m_session->game ? "game" : "archive");
    settings.setValue("session/game", m_session->game ? toQString(m_session->game->info().id) : QString());
    settings.setValue("session/path", toQString(m_session->source));
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

    m_pendingSelect = settings.value("session/selection").toString();
    if (kind == "game") {
        const Game* game = m_games.find(settings.value("session/game").toString().toStdString());
        if (!game)
            return false;
        openGame(*game, toPath(path));
        return true;
    }
    if (kind == "archive") {
        openArchive(toPath(path));
        return true;
    }
    return false;
}

void MainWindow::reloadSession(const QString& virtualPath)
{
    if (!m_session)
        return;
    m_pendingSelect = virtualPath;
    if (m_session->game)
        openGame(*m_session->game, m_session->source);
    else
        openArchive(m_session->source);
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
    updateBackupBanner();
    updateActions();
}

const VfsNode* MainWindow::currentNode() const
{
    return m_session ? VfsModel::nodeFromIndex(m_filter->mapToSource(m_tree->currentIndex())) : nullptr;
}

void MainWindow::updateBackupBanner()
{
    const VfsNode* node = currentNode();
    const auto target = node ? backupTarget(*node) : std::filesystem::path{};
    // Only while editing is possible, and only for things that can actually be backed up.
    const bool warn = m_editMode && !target.empty() && !hasBackup(*node);
    m_preview->setBackupWarning(warn ? toQString(target.filename()) : QString());
}

void MainWindow::backupCurrent()
{
    const VfsNode* node = currentNode();
    if (!node || m_editWatcher->isRunning())
        return;
    const auto target = backupTarget(*node);
    if (target.empty() || hasBackup(*node))
        return;
    m_preview->setBackupInProgress(true);
    m_statusLabel->setText(tr("Backing up %1…").arg(toQString(target.filename())));
    m_progress->setRange(0, 0);
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_editWatcher->setFuture(startBackup(target));
    updateActions();
}

// ---- Replacing assets -------------------------------------------------------------------------

void MainWindow::replaceSelected()
{
    const auto nodes = selectedNodes();
    if (nodes.size() != 1 || !Vfs::canReplace(*nodes.front()))
        return;
    const VfsNode* node = nodes.front();
    const QString ext = toQString(extensionOf(node->name()));
    QSettings settings;
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Replace %1 with…").arg(toQString(node->name())), settings.value("paths/lastReplaceDir").toString(),
        tr("%1 files (*.%2)").arg(ext.toUpper(), ext));
    if (file.isEmpty())
        return;
    settings.setValue("paths/lastReplaceDir", QFileInfo(file).absolutePath());
    replaceNode(node, file);
}

void MainWindow::replaceNode(const VfsNode* node, const QString& file)
{
    if (!m_session || !node || !m_editMode || m_editWatcher->isRunning() || m_loadWatcher->isRunning())
        return;

    auto plan = planReplace(*m_session, *node, file);
    if (!plan) {
        QMessageBox::warning(this, tr("Replace"), toQString(plan.error().message));
        return;
    }

    const QLocale locale;
    const QString where = node->archive() ? tr("inside <b>%1</b>").arg(toQString(node->archive()->path().filename()))
                                          : tr("on disk");
    QString text = tr("Replace <b>%1</b> %2 with <b>%3</b>?<br><br>Size: %4 → %5")
                       .arg(toQString(node->name()).toHtmlEscaped(), where, QFileInfo(file).fileName().toHtmlEscaped(),
                            locale.formattedDataSize(static_cast<qint64>(node->size())),
                            locale.formattedDataSize(static_cast<qint64>(plan->data.size())));
    if (!plan->warning.isEmpty())
        text += QStringLiteral("<br><br>⚠ ") + plan->warning.toHtmlEscaped();

    // No automatic backups: if there isn't one, say so and offer to make it.
    const auto target = Vfs::editTarget(*node);
    const bool backedUp = hasBackup(*node);
    QMessageBox box(QMessageBox::Question, tr("Replace"), QString(), QMessageBox::Cancel, this);
    QPushButton* replace = nullptr;
    QPushButton* backupAndReplace = nullptr;
    if (backedUp) {
        text += tr("<br><br>The original is backed up as %1.").arg(toQString(backupPathFor(target).filename()));
        replace = box.addButton(tr("Replace"), QMessageBox::AcceptRole);
    } else {
        box.setIcon(QMessageBox::Warning);
        text += tr("<br><br><b style='color:#C42B1C'>%1 is not backed up — this change can't be undone.</b>")
                    .arg(toQString(target.filename()).toHtmlEscaped());
        backupAndReplace = box.addButton(tr("Back Up && Replace"), QMessageBox::AcceptRole);
        replace = box.addButton(tr("Replace Without Backup"), QMessageBox::DestructiveRole);
    }
    box.setText(text);
    if (!plan->warning.isEmpty())
        box.setDefaultButton(QMessageBox::Cancel);
    else
        box.setDefaultButton(backupAndReplace ? backupAndReplace : replace);
    box.exec();
    if (box.clickedButton() == backupAndReplace && backupAndReplace)
        plan->backupFirst = true;
    else if (box.clickedButton() != replace)
        return;

    m_pendingSelect = toQString(node->virtualPath());
    m_statusLabel->setText(tr("Replacing %1…").arg(toQString(node->name())));
    m_progress->setRange(0, 0);
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_editWatcher->setFuture(startReplace(m_session, std::move(*plan)));
    updateActions();
}

void MainWindow::restoreOriginal()
{
    const auto nodes = selectedNodes();
    if (!m_session || !m_editMode || nodes.size() != 1 || !hasBackup(*nodes.front()) || m_editWatcher->isRunning())
        return;
    const VfsNode* node = nodes.front();
    const auto target = backupTarget(*node);
    const QString name = toQString(target.filename());
    if (QMessageBox::question(this, tr("Restore Original"),
                              tr("Restore the original %1 from %2?\n\nEvery replacement made in it since the backup "
                                 "will be undone.")
                                  .arg(name, toQString(backupPathFor(target).filename())))
        != QMessageBox::Yes)
        return;

    // Close the session first so no open handle blocks overwriting the file;
    // remember what to reopen once the restore finishes.
    m_reopen = {m_session->game, m_session->source};
    const QString reselect = toQString(node->virtualPath());
    setSession(nullptr);
    m_pendingSelect = reselect;

    m_statusLabel->setText(tr("Restoring %1…").arg(name));
    m_progress->setRange(0, 0);
    m_progress->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_editWatcher->setFuture(startRestore(target));
    updateActions();
}

void MainWindow::onEditFinished()
{
    QApplication::restoreOverrideCursor();
    m_progress->hide();
    const EditResult result = m_editWatcher->result();

    if (result.kind == EditResult::Kind::Backup) { // nothing changed in the archive: no reload
        m_preview->setBackupInProgress(false);
        if (!result.error.isEmpty())
            QMessageBox::warning(this, tr("Back Up"), result.error);
        else
            statusBar()->showMessage(result.summary, 15000);
        updateBackupBanner();
        updateActions();
        return;
    }

    if (!result.error.isEmpty()) {
        QMessageBox::warning(this, appName(), result.error);
        m_statusLabel->setText(tr("Edit failed"));
    }
    if (m_reopen) { // a restore closed the session: open it again
        const auto [game, source] = *std::exchange(m_reopen, std::nullopt);
        if (game)
            openGame(*game, source);
        else
            openArchive(source);
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

const VfsNode* MainWindow::dropTarget(const QPoint& pos, const QMimeData* mime, QString* reason) const
{
    if (!m_session || !mime || !mime->hasUrls() || mime->urls().size() != 1)
        return nullptr;
    const QString file = mime->urls().front().toLocalFile();
    if (!QFileInfo(file).isFile())
        return nullptr;
    const VfsNode* node = VfsModel::nodeFromIndex(m_filter->mapToSource(m_tree->indexAt(pos)));
    if (!node || !node->isFile())
        return nullptr;
    if (!m_editMode) {
        *reason = tr("Turn on Edit Mode (Ctrl+M) to replace files");
        return nullptr;
    }
    if (!Vfs::canReplace(*node)) {
        *reason = tr("%1 can't be replaced").arg(toQString(node->name()));
        return nullptr;
    }
    const QString targetExt = toQString(extensionOf(node->name()));
    if (QFileInfo(file).suffix().compare(targetExt, Qt::CaseInsensitive) != 0) {
        *reason = tr("Can't drop a .%1 file onto %2 (needs .%3)")
                      .arg(QFileInfo(file).suffix(), toQString(node->name()), targetExt);
        return nullptr;
    }
    return node;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_tree->viewport())
        return QMainWindow::eventFilter(watched, event);

    // Over the tree, a single dropped file either replaces the item under the
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
        if (const VfsNode* node = dropTarget(pos, drag->mimeData(), &reason)) {
            drag->acceptProposedAction();
            // Highlight the row that would be replaced (selection only, no preview reload).
            m_tree->selectionModel()->select(m_tree->indexAt(pos),
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
        const VfsNode* node = dropTarget(drop->position().toPoint(), drop->mimeData(), &reason);
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
    std::vector<const VfsNode*> nodes;
    for (const QModelIndex& index : m_tree->selectionModel()->selectedRows(VfsModel::Name))
        if (const VfsNode* node = VfsModel::nodeFromIndex(m_filter->mapToSource(index)))
            nodes.push_back(node);
    return nodes;
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

    m_openGameAction->setEnabled(!busy);
    m_openArchiveAction->setEnabled(!busy);
    m_extractAction->setEnabled(hasSession && !nodes.empty() && !extracting);
    m_extractAllAction->setEnabled(hasSession && !extracting);
    m_editModeAction->setEnabled(!editing);
    m_replaceAction->setEnabled(m_editMode && !busy && single && Vfs::canReplace(*single));
    m_restoreAction->setEnabled(m_editMode && !busy && single && hasBackup(*single));
    m_copyPathAction->setEnabled(!nodes.empty());
    m_revealAction->setEnabled(single && !single->diskPath().empty());
}

void MainWindow::showTreeContextMenu(const QPoint& pos)
{
    if (!m_tree->indexAt(pos).isValid())
        return;
    QMenu menu(this);
    menu.addAction(m_extractAction);
    menu.addAction(m_replaceAction);
    if (m_restoreAction->isEnabled())
        menu.addAction(m_restoreAction);
    menu.addSeparator();
    menu.addAction(m_copyPathAction);
    menu.addAction(m_revealAction);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ---- Extraction ------------------------------------------------------------------------------

void MainWindow::extract(bool everything)
{
    if (!m_session || m_extractWatcher->isRunning())
        return;
    std::vector<ExtractItem> items;
    if (everything) {
        for (const VfsNode* file : Vfs::collectFiles(m_session->vfs.root()))
            items.push_back({file, file->virtualPath()});
    } else {
        items = planExtraction(selectedNodes());
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
        m_statusLabel->setText(tr("Extraction cancelled"));
        return;
    }
    const ExtractSummary summary = m_extractWatcher->result();
    const QString link = QUrl::fromLocalFile(m_extractTarget).toString();
    QString message = summary.canceled ? tr("Extraction cancelled after %n file(s)", nullptr, summary.written)
                                       : tr("Extracted %n file(s)", nullptr, summary.written);
    message += QStringLiteral(" (%1) — <a href=\"%2\">%3</a>").arg(sizeText(summary.bytes), link, tr("Open folder"));
    m_statusLabel->setText(message);

    if (summary.failed > 0) {
        QMessageBox box(QMessageBox::Warning, appName(),
                        tr("%n file(s) could not be extracted.", nullptr, summary.failed), QMessageBox::Ok, this);
        box.setDetailedText(summary.errors.join('\n'));
        box.exec();
    }
}

// ---- Window events ----------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_extractWatcher->isRunning()) {
        m_extractWatcher->cancel();
        m_extractWatcher->waitForFinished();
    }
    QSettings settings;
    // Reopened at next start-up (see reopenLastSession).
    const VfsNode* current = VfsModel::nodeFromIndex(m_filter->mapToSource(m_tree->currentIndex()));
    settings.setValue("session/selection", current ? toQString(current->virtualPath()) : QString());
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("window/treeHeader", m_tree->header()->saveState());
    if (auto* splitter = findChild<QSplitter*>("mainSplitter"))
        settings.setValue("window/splitter", splitter->saveState());
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
