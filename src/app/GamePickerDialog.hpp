#pragma once

#include "viewer/core/GameRegistry.hpp"

#include <QDialog>

#include <filesystem>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace viewer::app {

/// Startup dialog: pick which game to browse and where it is installed.
/// Remembers the last game and each game's folder between runs.
class GamePickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit GamePickerDialog(const GameRegistry& games, QWidget* parent = nullptr);

    [[nodiscard]] const Game* selectedGame() const;
    [[nodiscard]] std::filesystem::path installPath() const;
    /// True if the user chose "Open archive file…" instead of a game.
    [[nodiscard]] bool wantsArchive() const noexcept { return m_wantsArchive; }

    void accept() override;

private:
    void onGameChanged();
    void validate();
    void browse();

    const GameRegistry& m_games;
    QListWidget* m_list;
    QLineEdit* m_path;
    QLabel* m_status;
    QPushButton* m_open;
    bool m_wantsArchive = false;
};

} // namespace viewer::app
