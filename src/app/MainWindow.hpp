#pragma once

#include "Extractor.hpp"
#include "Session.hpp"

#include <QFutureWatcher>
#include <QMainWindow>

#include <functional>
#include <memory>

class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace viewer::app {

class PreviewPane;
class ThemeManager;
class VfsFilterModel;
class VfsModel;

/// Outcome of a background open (game folder or archive).
struct LoadResult {
    std::shared_ptr<Session> session;
    QString error;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const GameRegistry& games, ThemeManager& theme, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Shows the game picker; returns false if the user cancelled.
    bool chooseGame();
    /// Opens a path given on the command line (game folder or archive).
    void openPath(const QString& path);
    /// Selects and reveals a node by virtual path ("cache/root.pak/models/a.model").
    bool selectPath(const QString& virtualPath);
    [[nodiscard]] PreviewPane* previewPane() const noexcept { return m_preview; }

signals:
    /// A game or archive finished loading (or the session was closed).
    void sessionChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildActions();
    void buildUi();
    void openGame(const Game& game, const std::filesystem::path& directory);
    void openArchiveDialog();
    void openArchive(const std::filesystem::path& file);
    void startLoad(const QString& message, std::function<LoadResult()> job);
    void onLoadFinished();
    void setSession(std::shared_ptr<Session> session);
    QWidget* buildPlaceholder();
    void configureTreeHeader();
    void showHeaderMenu(const QPoint& pos);

    void extract(bool everything);
    void onExtractFinished();
    void showTreeContextMenu(const QPoint& pos);
    [[nodiscard]] std::vector<const VfsNode*> selectedNodes() const;
    void updateActions();
    void showAbout();

    const GameRegistry& m_games;
    ThemeManager& m_theme;
    std::shared_ptr<Session> m_session;
    QFutureWatcher<LoadResult>* m_loadWatcher;
    QFutureWatcher<ExtractSummary>* m_extractWatcher;
    QString m_extractTarget;

    VfsModel* m_model;
    VfsFilterModel* m_filter;
    QStackedWidget* m_leftStack;
    QTreeView* m_tree;
    QWidget* m_placeholder;
    PreviewPane* m_preview;
    QLineEdit* m_search;
    QLabel* m_statusLabel;
    QProgressBar* m_progress;
    QToolButton* m_cancel;

    QAction* m_openGameAction = nullptr;
    QAction* m_openArchiveAction = nullptr;
    QAction* m_extractAction = nullptr;
    QAction* m_extractAllAction = nullptr;
    QAction* m_copyPathAction = nullptr;
    QAction* m_revealAction = nullptr;
};

} // namespace viewer::app
