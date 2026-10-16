#pragma once

#include "Session.hpp"

#include <QDialog>

#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace viewer::app {

/// The game's names <-> numbers (its HashTool): type a name, a text or a
/// number and see what the game uses for it. One window, kept by the main
/// window and given each new session.
class HashWindow : public QDialog {
    Q_OBJECT
public:
    explicit HashWindow(QWidget* parent = nullptr);

    void setSession(std::shared_ptr<Session> session);
    /// Puts `text` in the search box and looks it up.
    void search(const QString& text);

private:
    void lookup();
    void copySelected();

    std::shared_ptr<Session> m_session;
    QLabel* m_about;
    QLineEdit* m_query;
    QTreeWidget* m_results;
    QPushButton* m_copy;
    QTimer* m_typing;
};

} // namespace viewer::app
