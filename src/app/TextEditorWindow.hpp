#pragma once

#include "Session.hpp"

#include "viewer/core/Text.hpp"

#include <QMainWindow>

#include <memory>

namespace viewer {
class TextDecompiler;
}

class QAction;
class QComboBox;
class QLabel;

namespace viewer::app {

class TextView;

/// Edits one text file (opened with "Edit" from a preview or the right-click
/// menu). Saving hands the encoded bytes to the main window, which writes them
/// back into the game through the usual Edit Mode / backup / replace flow.
///
/// Binary files a decompiler can compile back (Blur's data tables, text
/// tables…) are edited as their decompiled text: saving turns the edited
/// values back into the file.
///
/// Like PreviewWindow it remembers its file's location, so after the session
/// reloads (e.g. after saving) it is pointed at the new node; the text being
/// edited is kept either way.
class TextEditorWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit TextEditorWindow(QWidget* parent = nullptr);
    ~TextEditorWindow() override;

    /// Loads `node`'s text. Returns an error message if it can't be edited as text.
    [[nodiscard]] QString open(std::shared_ptr<Session> session, const VfsNode* node, const QString& location);
    /// Points the window at `node` in a new session (nullptr: the file is gone),
    /// keeping the text being edited.
    void rebind(std::shared_ptr<Session> session, const VfsNode* node);

    [[nodiscard]] const VfsNode* node() const noexcept { return m_node; }
    [[nodiscard]] const QString& location() const noexcept { return m_location; }
    [[nodiscard]] bool isModified() const;

    /// The main window finished a save: `ok` means the game file now holds the text.
    void saveFinished(bool ok);

signals:
    /// Write `bytes` (the encoded text) to this window's file.
    void saveRequested(const QByteArray& bytes);
    void revealRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void save();
    void saveCopy();
    [[nodiscard]] Result<ByteBuffer> encoded() const;
    void updateTitle();
    void updateActions();
    void updatePosition();

    /// Jumps to the line an error message names ("line 12: …").
    void showErrorLine(const QString& message);

    std::shared_ptr<Session> m_session;
    const VfsNode* m_node = nullptr;
    const TextDecompiler* m_decompiler = nullptr; ///< Set when editing a decompiled binary file.
    QString m_location;
    QString m_name;
    bool m_saving = false;
    bool m_closeAfterSave = false;

    TextView* m_view;
    QLabel* m_hint; ///< What can be changed, when editing decompiled text.
    QAction* m_saveAction;
    QAction* m_saveCopyAction;
    QAction* m_undoAction;
    QAction* m_redoAction;
    QAction* m_revealAction;
    QComboBox* m_encoding;
    QComboBox* m_lineEnding;
    QLabel* m_position;
};

} // namespace viewer::app
