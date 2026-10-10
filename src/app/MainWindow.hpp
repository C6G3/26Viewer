#pragma once

#include "AssetEditor.hpp"
#include "Extractor.hpp"
#include "Session.hpp"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>

class QAbstractItemView;
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
class PreviewWindow;
class TextEditorWindow;
class ThemeManager;
class VfsFilterModel;
class VfsModel;

/// Outcome of a background open (game folder or archive).
struct LoadResult {
    std::shared_ptr<Session> session;
    QString error;
    QStringList warnings; ///< Extra mounts that couldn't be reopened.
};

/// Outcome of opening an archive/folder alongside the current session.
struct MountResult {
    const Session* session = nullptr; ///< The session it was mounted for.
    std::shared_ptr<Session::Extra> extra;
    QString error;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const GameRegistry& games, ThemeManager& theme, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Shows the game picker; returns false if the user cancelled.
    bool chooseGame();
    /// Opens a dropped / command-line path: a game install opens as that game;
    /// other folders and archives open alongside the current game (or on
    /// their own if nothing is open).
    void openPath(const QString& path);
    /// Reveals a node by location — a virtual path in the main mount
    /// ("cache/root.pak/models/a.model"), or "<source>|<path>" for an extra
    /// mount. A folder opens in the list; a file is selected in its folder and,
    /// with `openFile`, opened in a preview window.
    bool selectPath(const QString& location, bool openFile = true);
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
    /// Automation (--screenshot): preview windows are rendered off-screen too.
    void setOffscreen(bool offscreen) noexcept { m_offscreen = offscreen; }

signals:
    /// A game or archive finished loading (or the session was closed).
    void sessionChanged();
    /// A new preview window was opened (before its file finishes loading).
    void previewOpened(PreviewWindow* window);
    /// A text editor window was opened.
    void editorOpened(TextEditorWindow* window);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildActions();
    void buildUi();
    void openGame(const Game& game, const std::filesystem::path& directory);
    /// Loads a new session (game, or else folder / archive) with `extras`
    /// mounted alongside, then selects `select`.
    void openSource(const Game* game, const std::filesystem::path& source,
                    std::vector<std::filesystem::path> extras, const QString& select);
    void openArchiveDialog();
    void openFolderDialog();
    /// Settings key listing the archives/folders opened alongside `game`
    /// (per game; standalone sessions share one list).
    [[nodiscard]] static QString extrasKey(const Game* game);
    void openArchive(const std::filesystem::path& file);
    void openFolder(const std::filesystem::path& directory);
    /// Opens an archive or folder alongside the current session (in the background).
    void addMount(const std::filesystem::path& source);
    void onMountFinished();
    /// Closes an archive/folder that was opened alongside the game.
    void closeMount(const VfsNode* top);
    void startLoad(const QString& message, std::function<LoadResult()> job);
    void onLoadFinished();
    void setSession(std::shared_ptr<Session> session);
    QWidget* buildPlaceholder();

    // Browsing: tree (folders/archives) → folder list → preview windows
    /// Shows `folder` in the list (selecting it in the tree). With `reveal`,
    /// selects the item in `folder` that leads to it (used when going up).
    void navigateTo(const VfsNode* folder, const VfsNode* reveal = nullptr);
    /// Double-click / Enter in the list: enter folders, preview files.
    void activateNode(const VfsNode* node);
    /// Opens `node` in a preview window (or brings its open window forward).
    PreviewWindow* openPreview(const VfsNode* node, bool properties = false);
    /// Backspace: up to the parent folder.
    void goBack();
    [[nodiscard]] const VfsNode* treeNode() const;
    /// See selectPath for the format.
    [[nodiscard]] QString locationOf(const VfsNode* node) const;
    [[nodiscard]] const VfsNode* findLocation(const QString& location) const;
    [[nodiscard]] std::vector<PreviewWindow*> previewWindows() const;

    // Text editing (TextEditorWindow)
    /// Whether `node` can be opened in the text editor; with `sniff`, files of
    /// unknown type are read to check that they look like text.
    [[nodiscard]] bool isEditableText(const VfsNode* node, bool sniff) const;
    /// Opens `node` in a text editor window (or brings its open one forward).
    void openTextEditor(const VfsNode* node);
    [[nodiscard]] std::vector<TextEditorWindow*> editorWindows() const;
    /// Writes an editor's text into the game (asks for Edit Mode and confirmation).
    void saveEditedText(TextEditorWindow* editor, const QByteArray& bytes);

    void extract(bool everything);
    void extractNodes(const std::vector<const VfsNode*>& nodes);
    void onExtractFinished();

    // Replacing assets (see AssetEditor)
    void replaceSelected();
    /// Asks for a replacement file for `node`, then replaces it.
    void replaceWithDialog(const VfsNode* node, QWidget* dialogParent);
    void replaceNode(const VfsNode* node, const QString& file, QWidget* dialogParent = nullptr);
    /// Confirms a checked replacement with the user (backup choice) and starts it.
    /// `edited`: the new contents are the user's own edits rather than a file.
    bool confirmAndReplace(ReplacePlan plan, bool edited, QWidget* dialogParent);
    /// Why `file` can't replace `node` (empty if it can).
    [[nodiscard]] QString replaceRefusal(const VfsNode* node, const QString& file) const;
    void restoreOriginal();
    void onEditFinished();
    /// Re-mounts the session (with its extra mounts) and then selects `location`.
    void reloadSession(const QString& location);
    [[nodiscard]] const VfsNode* nodeAt(const QAbstractItemView* view, const QPoint& pos) const;
    /// The file node a drag at `pos` in `view` would replace; `reason` explains a refusal.
    const VfsNode* dropTarget(const QAbstractItemView* view, const QPoint& pos, const QMimeData* mime,
                              QString* reason) const;
    void rememberSession() const;
    /// Shows/hides the red "not backed up" banners (folder list and preview windows).
    void updateBackupBanner();
    /// Backs up `node`'s archive (or loose file) in the background.
    void backupNode(const VfsNode* node);

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
    struct Reopen {
        const Game* game = nullptr; ///< Null for a standalone archive or folder.
        std::filesystem::path source;
        std::vector<std::filesystem::path> extras;
    };
    std::optional<Reopen> m_reopen; ///< What to reopen after a restore closed the session.

    VfsModel* m_model;
    VfsFilterModel* m_treeFilter; ///< Folders and archives only.
    QStackedWidget* m_leftStack;
    QTreeView* m_tree;
    QWidget* m_placeholder;
    FolderView* m_folderView;
    std::vector<QPointer<PreviewWindow>> m_previews; ///< Open preview windows (null once closed).
    std::vector<QPointer<TextEditorWindow>> m_editors; ///< Open text editors (null once closed).
    QPointer<TextEditorWindow> m_savingEditor; ///< The editor whose save is running.
    QLineEdit* m_search;
    QLabel* m_statusLabel;
    QProgressBar* m_progress;
    QToolButton* m_cancel;
    QFutureWatcher<MountResult>* m_mountWatcher;

    QAction* m_openGameAction = nullptr;
    QAction* m_openArchiveAction = nullptr;
    QAction* m_openFolderAction = nullptr;
    QAction* m_extractAction = nullptr;
    QAction* m_extractAllAction = nullptr;
    QAction* m_copyPathAction = nullptr;
    QAction* m_revealAction = nullptr;
    QAction* m_replaceAction = nullptr;
    QAction* m_restoreAction = nullptr;
    QAction* m_editTextAction = nullptr;
    QAction* m_reopenAction = nullptr;
    QAction* m_editModeAction = nullptr;
    QAction* m_backAction = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_propertiesAction = nullptr;
    QLabel* m_editModeBadge = nullptr;
    bool m_editMode = false;
    bool m_rememberSession = true;
    bool m_offscreen = false;
};

} // namespace viewer::app
