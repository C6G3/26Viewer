#pragma once

#include "AssetEditor.hpp"
#include "Extractor.hpp"
#include "Session.hpp"

#include <QFutureWatcher>
#include <QMainWindow>

#include <functional>
#include <memory>
#include <optional>

class QAction;
class QLabel;
class QMimeData;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace viewer::app {

class FolderView;
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
    /// Reveals a node by virtual path ("cache/root.pak/models/a.model"): a
    /// folder/archive is opened in the list; a file is selected in its folder
    /// and, with `openFile`, previewed.
    bool selectPath(const QString& virtualPath, bool openFile = true);
    [[nodiscard]] PreviewPane* previewPane() const noexcept { return m_preview; }
    /// True while a file's preview (rather than the folder list) is shown.
    [[nodiscard]] bool previewShowing() const;
    /// Re-opens the game/archive from the previous run (saved in the settings
    /// file in %APPDATA%). Returns false if there is nothing to reopen or the
    /// user turned the option off.
    bool reopenLastSession();

    /// Edit Mode unlocks replacing/restoring (off at every start, like OpenIV).
    /// With `confirm`, the user must accept a warning before it turns on.
    void setEditMode(bool on, bool confirm = true);
    [[nodiscard]] bool editMode() const noexcept { return m_editMode; }

    /// Automation runs (--screenshot) pass false so they don't overwrite the
    /// user's remembered session.
    void setRememberSession(bool remember) noexcept { m_rememberSession = remember; }

signals:
    /// A game or archive finished loading (or the session was closed).
    void sessionChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
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

    // Browsing: tree (folders/archives) → folder list → preview
    /// Shows `folder` in the list (selecting it in the tree). With `reveal`,
    /// selects the item in `folder` that leads to it (used when going up).
    void navigateTo(const VfsNode* folder, const VfsNode* reveal = nullptr);
    /// Double-click / Enter in the list: enter folders, preview files.
    void activateNode(const VfsNode* node);
    void openPreview(const VfsNode* node, bool properties = false);
    /// Returns from the preview to the folder list.
    void showList();
    /// Backspace: preview → list, list → parent folder.
    void goBack();
    [[nodiscard]] const VfsNode* treeNode() const;

    void extract(bool everything);
    void onExtractFinished();

    // Replacing assets (see AssetEditor)
    void replaceSelected();
    void replaceNode(const VfsNode* node, const QString& file);
    void restoreOriginal();
    void onEditFinished();
    /// Re-mounts the current game/archive and then selects `virtualPath`
    /// (see m_pendingOpen).
    void reloadSession(const QString& virtualPath);
    /// The file node a drag at `pos` would replace; `reason` explains a refusal.
    const VfsNode* dropTarget(const QPoint& pos, const QMimeData* mime, QString* reason) const;
    void rememberSession() const;
    /// Shows/hides the preview's red "not backed up" banner for the current item.
    void updateBackupBanner();
    /// Backs up the current item's archive (or loose file) in the background.
    void backupCurrent();
    /// What the backup banner refers to: the previewed file, else the open folder.
    [[nodiscard]] const VfsNode* currentNode() const;

    void showTreeContextMenu(const QPoint& pos);
    void showListContextMenu(const QPoint& globalPos);
    [[nodiscard]] std::vector<const VfsNode*> selectedNodes() const;
    void updateActions();
    void showAbout();

    const GameRegistry& m_games;
    ThemeManager& m_theme;
    std::shared_ptr<Session> m_session;
    QFutureWatcher<LoadResult>* m_loadWatcher;
    QFutureWatcher<ExtractSummary>* m_extractWatcher;
    QFutureWatcher<EditResult>* m_editWatcher;
    QString m_extractTarget;
    QString m_pendingSelect; ///< Selected once the next session finishes loading.
    bool m_pendingOpen = true; ///< ...and previewed, if it is a file.
    struct Reopen {
        const Game* game = nullptr; ///< Null for a standalone archive.
        std::filesystem::path source;
    };
    std::optional<Reopen> m_reopen; ///< What to reopen after a restore closed the session.

    VfsModel* m_model;
    VfsFilterModel* m_treeFilter; ///< Folders and archives only.
    QStackedWidget* m_leftStack;
    QTreeView* m_tree;
    QWidget* m_placeholder;
    QStackedWidget* m_rightStack; ///< Folder list or preview.
    FolderView* m_folderView;
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
    QAction* m_replaceAction = nullptr;
    QAction* m_restoreAction = nullptr;
    QAction* m_reopenAction = nullptr;
    QAction* m_editModeAction = nullptr;
    QAction* m_backAction = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_propertiesAction = nullptr;
    QLabel* m_editModeBadge = nullptr;
    bool m_editMode = false;
    bool m_rememberSession = true;
};

} // namespace viewer::app
