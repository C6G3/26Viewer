#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace viewer::app {

class ThemeManager;

/// The options in Edit ▸ Settings, stored in 26Viewer.ini. Read them with
/// load() where they are used, so a change applies from the next action on.
struct AppSettings {
    QString extractFolder;   ///< Extract / Export / Save dialogs start here; empty: the last folder used.
    int extractThreads = 4;  ///< Files extracted at once.
    int exportThreads = 2;   ///< Files exported (converted) at once.
    int packedPreviewMB = 512; ///< Files in archives up to this size are previewed (read whole).
    int looseReadMB = 64;      ///< Loose files: how much is read for the preview and hex view.
    int textMB = 4;            ///< Text shown in the preview.

    [[nodiscard]] static AppSettings load();
    void save() const;

    [[nodiscard]] std::uint64_t packedPreviewLimit() const noexcept { return std::uint64_t(packedPreviewMB) << 20; }
    [[nodiscard]] std::uint64_t looseReadLimit() const noexcept { return std::uint64_t(looseReadMB) << 20; }
    [[nodiscard]] std::uint64_t textLimit() const noexcept { return std::uint64_t(textMB) << 20; }

    /// The folder an Extract / Export / Save dialog starts in.
    [[nodiscard]] static QString startFolder();
};

/// Edit ▸ Settings: appearance and start-up, extracting, previews.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ThemeManager& theme, QWidget* parent = nullptr);

    /// Saves the choices (and applies the theme).
    void accept() override;

private:
    void restoreDefaults();
    void fill(const AppSettings& s);

    ThemeManager& m_theme;
    QComboBox* m_themeBox;
    QCheckBox* m_reopen;
    QRadioButton* m_lastFolder;
    QRadioButton* m_fixedFolder;
    QLineEdit* m_folder;
    QSpinBox* m_extractThreads;
    QSpinBox* m_exportThreads;
    QSpinBox* m_packedPreview;
    QSpinBox* m_looseRead;
    QSpinBox* m_text;
};

} // namespace viewer::app
