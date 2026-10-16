#pragma once

#include "Session.hpp"

#include <QDialog>
#include <QElapsedTimer>

#include <memory>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTabWidget;
class QTimer;
class QTreeWidget;

namespace viewer::app {

/// Tools ▸ AI Assistant (MCP)…: how to connect an AI assistant (Claude Code,
/// Claude Desktop, VS Code…) to 26viewer-cli's MCP server for the open game —
/// ready-made settings to copy, and a test that starts the server the way an
/// assistant would. The app never changes another program's settings itself.
/// One window, kept by the main window and given each new session.
class McpWindow : public QDialog {
    Q_OBJECT
public:
    explicit McpWindow(QWidget* parent = nullptr);
    ~McpWindow() override;

    void setSession(std::shared_ptr<Session> session);
    /// Starts the server, asks it what it offers, and shows the answer.
    void test();

private:
    /// The page for one assistant: where the text goes, the text, and a Copy button.
    QPlainTextEdit* addPage(QTabWidget* tabs, const QString& title, QLabel*& hint);
    void refresh();
    void testFinished();

    [[nodiscard]] QString program() const;
    [[nodiscard]] QString target() const;
    [[nodiscard]] QStringList arguments() const;

    std::shared_ptr<Session> m_session;
    QLabel* m_program;
    QLabel* m_target;
    QCheckBox* m_allowEdits;
    QLabel* m_codeHint = nullptr;
    QLabel* m_desktopHint = nullptr;
    QLabel* m_vscodeHint = nullptr;
    QLabel* m_otherHint = nullptr;
    QPlainTextEdit* m_code;
    QPlainTextEdit* m_desktop;
    QPlainTextEdit* m_vscode;
    QPlainTextEdit* m_other;
    QPushButton* m_test;
    QLabel* m_status;
    QTreeWidget* m_tools;
    QProcess* m_process = nullptr;
    QTimer* m_timeout;
    QElapsedTimer m_started;
};

} // namespace viewer::app
