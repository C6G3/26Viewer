#pragma once

#include <QFrame>

class QLabel;
class QPushButton;

namespace viewer::app {

/// The red "⚠ X is not backed up" strip with a "Back Up Now" button, shown in
/// Edit Mode above the folder list and the preview.
class BackupBanner : public QFrame {
    Q_OBJECT
public:
    explicit BackupBanner(QWidget* parent = nullptr);

    /// Shows the warning for `fileName`; an empty name hides the banner.
    void setWarning(const QString& fileName);
    /// Disables the button while a backup is running.
    void setInProgress(bool running);

signals:
    void backupRequested();

private:
    QLabel* m_text;
    QPushButton* m_button;
};

} // namespace viewer::app
