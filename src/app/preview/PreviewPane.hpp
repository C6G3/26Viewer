#pragma once

#include "../Session.hpp"

#include <QFutureWatcher>
#include <QWidget>

#include <memory>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;

namespace viewer::app {

class HexView;
class ImageView;
struct PreviewResult;

/// Right-hand side of the main window: Preview / Structure / Hex / Properties.
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

signals:
    /// The current node's preview has been fully populated.
    void previewReady();

private:
    enum Tab { PreviewTab, StructureTab, HexTab, PropertiesTab };

    void showMessage(const QString& message);
    void applyResult(const PreviewResult& result);
    void fillProperties(const VfsNode* node, const PreviewResult* result);

    std::shared_ptr<Session> m_session;
    const VfsNode* m_node = nullptr;
    quint64 m_generation = 0;
    int m_preferredTab = PreviewTab; ///< Last tab the user clicked.
    QFutureWatcher<std::shared_ptr<PreviewResult>>* m_watcher;

    QLabel* m_title;
    QLabel* m_subtitle;
    QTabWidget* m_tabs;
    QStackedWidget* m_previewStack;
    QLabel* m_message;
    ImageView* m_image;
    QPlainTextEdit* m_text;
    QTreeWidget* m_structure;
    HexView* m_hex;
    QTreeWidget* m_properties;
};

} // namespace viewer::app
