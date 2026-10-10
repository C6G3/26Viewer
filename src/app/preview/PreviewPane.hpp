#pragma once

#include "../Session.hpp"

#include <QFutureWatcher>
#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;

namespace viewer::app {

class BackupBanner;
class HexView;
class ImageView;
class ModelView;
class SoundBankView;
class TextView;
class VideoView;
struct PreviewResult;

/// The contents of a PreviewWindow: Preview / Structure / Hex / Properties.
///
/// Selecting a file starts a background job that reads, decompresses and
/// decodes it; stale results (the user already clicked something else) are
/// dropped using a generation counter.
class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget* parent = nullptr);
    ~PreviewPane() override;

    void setSession(std::shared_ptr<Session> session);
    void showNode(const VfsNode* node);
    /// 0 Preview, 1 Structure, 2 Hex, 3 Properties.
    void setCurrentTab(int tab);
    /// Shows `node` with the Properties tab in front (without changing the
    /// tab used for later files).
    void showProperties(const VfsNode* node);
    [[nodiscard]] const VfsNode* node() const noexcept { return m_node; }

    /// Shows the red "not backed up" banner for `fileName`; an empty name hides it.
    void setBackupWarning(const QString& fileName);
    /// Disables the banner's button while a backup is running.
    void setBackupInProgress(bool running);

signals:
    /// The current node's preview has been fully populated.
    void previewReady();
    /// The user clicked "Back Up Now" on the banner.
    void backupRequested();
    /// The user clicked "Edit" on a text file's preview.
    void editRequested();
    /// The user asked to replace sound `index` of the shown bank.
    void replaceSoundRequested(int index);

private:
    enum Tab { PreviewTab, StructureTab, HexTab, PropertiesTab };

    void display(const VfsNode* node, int forcedTab);
    void showMessage(const QString& message);
    void applyResult(const PreviewResult& result);
    void fillProperties(const VfsNode* node, const PreviewResult* result);

    std::shared_ptr<Session> m_session;
    const VfsNode* m_node = nullptr;
    quint64 m_generation = 0;
    int m_preferredTab = PreviewTab; ///< Last tab the user clicked.
    int m_forcedTab = -1;            ///< One-off tab for the current node (showProperties).
    QFutureWatcher<std::shared_ptr<PreviewResult>>* m_watcher;

    QLabel* m_title;
    QLabel* m_subtitle;
    BackupBanner* m_banner;
    QTabWidget* m_tabs;
    QStackedWidget* m_previewStack;
    QLabel* m_message;
    ImageView* m_image;
    ModelView* m_model;
    SoundBankView* m_bankView;
    VideoView* m_video;
    TextView* m_text;
    QTreeWidget* m_structure;
    HexView* m_hex;
    QTreeWidget* m_properties;
};

} // namespace viewer::app
