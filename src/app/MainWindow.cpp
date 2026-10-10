#include "MainWindow.hpp"

#include "GamePickerDialog.hpp"
#include "Icons.hpp"
#include "QtConvert.hpp"
#include "Theme.hpp"
#include "VfsModel.hpp"
#include "preview/PreviewPane.hpp"

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
      m_extractWatcher(new QFutureWatcher<ExtractSummary>(this)), m_model(new VfsModel(this)),
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

    // Menus
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_openGameAction);
    file->addAction(m_openArchiveAction);
    file->addSeparator();
    file->addAction(m_extractAction);
    file->addAction(m_extractAllAction);
    file->addSeparator();
    QAction* quit = file->addAction(icons::theme(TI::ApplicationExit, QStyle::SP_DialogCloseButton), tr("E&xit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
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
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

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
    configureTreeHeader();
    m_tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree->header(), &QHeaderView::customContextMenuRequested, this, &MainWindow::showHeaderMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &MainWindow::showTreeContextMenu);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        m_preview->showNode(VfsModel::nodeFromIndex(m_filter->mapToSource(index)));
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
    } else {
        m_leftStack->setCurrentWidget(m_placeholder);
        setWindowTitle(appName());
        m_statusLabel->setText(tr("Ready"));
    }
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
    const bool hasSession = static_cast<bool>(m_session);
    const auto nodes = hasSession ? selectedNodes() : std::vector<const VfsNode*>{};

    m_openGameAction->setEnabled(!loading && !extracting);
    m_openArchiveAction->setEnabled(!loading && !extracting);
    m_extractAction->setEnabled(hasSession && !nodes.empty() && !extracting);
    m_extractAllAction->setEnabled(hasSession && !extracting);
    m_copyPathAction->setEnabled(!nodes.empty());
    m_revealAction->setEnabled(nodes.size() == 1 && !nodes.front()->diskPath().empty());
}

void MainWindow::showTreeContextMenu(const QPoint& pos)
{
    if (!m_tree->indexAt(pos).isValid())
        return;
    QMenu menu(this);
    menu.addAction(m_extractAction);
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
