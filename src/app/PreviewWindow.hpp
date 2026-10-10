#pragma once

#include "Session.hpp"

#include <QMainWindow>

#include <functional>
#include <memory>

class QAction;
class QMimeData;

namespace viewer::app {

class PreviewPane;

/// A separate window showing one file (opened by double-clicking it in the
/// folder list), OpenIV-style. Several can be open at once. Its toolbar is the
/// place for per-file tools and adjustments.
///
/// The window remembers where its file lives (its location), so after the
/// session is reloaded (e.g. a replace) the main window can point it at the
/// new node.
class PreviewWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit PreviewWindow(QWidget* parent = nullptr);
    ~PreviewWindow() override;

    void setSession(std::shared_ptr<Session> session);
    /// Shows `node` (from the current session); `properties` opens the Properties tab.
    void showNode(const VfsNode* node, bool properties = false);
    [[nodiscard]] const VfsNode* node() const;
    /// Where the file lives (see MainWindow::selectPath); survives a reload.
    void setLocation(const QString& location) { m_location = location; }
    [[nodiscard]] const QString& location() const noexcept { return m_location; }
    [[nodiscard]] PreviewPane* pane() const noexcept { return m_pane; }

    /// Enables Replace (Edit Mode).
    void setEditMode(bool on);
    void setBackupWarning(const QString& fileName);
    void setBackupInProgress(bool running);
    /// Checks a file dropped on the window: returns why it can't replace this
    /// window's file, or an empty string if it can.
    void setDropCheck(std::function<QString(const QString& file)> check) { m_dropCheck = std::move(check); }

signals:
    /// Replace this window's file with `file` (empty: ask for one).
    void replaceRequested(const QString& file);
    void extractRequested();
    /// "Show in List": reveal the file in the main window.
    void revealRequested();
    void backupRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void updateTitle();
    [[nodiscard]] QString droppedFile(const QMimeData* mime) const;

    PreviewPane* m_pane;
    QString m_location;
    bool m_editMode = false;
    std::function<QString(const QString&)> m_dropCheck;

    QAction* m_revealAction;
    QAction* m_extractAction;
    QAction* m_replaceAction;
};

} // namespace viewer::app
