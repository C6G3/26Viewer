#pragma once

#include "Session.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QWidget>

#include <memory>
#include <vector>

class QLabel;
class QListWidgetItem;
class QMimeData;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace viewer::app {

class ImageView;

/// The textures inside one file (a "texture dictionary", e.g. a Blur model):
/// thumbnails on the left, the selected texture large on the right with its
/// size, pixel format and mip levels. Export (PNG/DDS/JPG/BMP) and Extract
/// (the stored pixels as .dds) work here; Replace and Import ask the main
/// window, which writes through Edit Mode like every other change. Pictures
/// dragged from Explorer replace the texture they are dropped on (one
/// picture), or the textures named after them (dropped anywhere else).
class TextureBrowser : public QWidget {
    Q_OBJECT
public:
    explicit TextureBrowser(QWidget* parent = nullptr);
    ~TextureBrowser() override;

    /// Shows the textures of `node` (reloaded after the file changes).
    void setSource(std::shared_ptr<Session> session, const VfsNode* node);
    /// Enables Replace / Import (Edit Mode).
    void setEditMode(bool on);
    [[nodiscard]] const VfsNode* node() const noexcept { return m_node; }

signals:
    /// Texture `index` should be replaced by `file` (empty: the user picks one).
    void replaceRequested(int index, const QString& file);
    /// Pictures matched to the textures by name (empty: from a folder the user picks).
    void importRequested(const QStringList& files);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct Entry {
        DictionaryTexture info;
        QImage image; ///< Most detailed level.
    };
    struct Loaded {
        const TextureDictionaryFormat* format = nullptr;
        ByteBuffer data;
        std::vector<Entry> entries;
        QString error;
    };

    void applyLoaded();
    void filter(const QString& text);
    void showCurrent();
    void updateButtons();
    [[nodiscard]] std::vector<int> selected() const;
    void exportTextures(const std::vector<int>& indices);
    void extractTextures(const std::vector<int>& indices);
    [[nodiscard]] QString details(const DictionaryTexture& t) const;
    /// The pictures (png/dds/jpg/bmp files) being dragged in, if any.
    [[nodiscard]] QStringList droppedPictures(const QMimeData* mime) const;
    /// The texture row under `pos` (window coordinates), if any.
    [[nodiscard]] QListWidgetItem* rowAt(const QPoint& pos) const;

    std::shared_ptr<Session> m_session;
    const VfsNode* m_node = nullptr;
    bool m_editMode = false;
    Loaded m_loaded;
    QFutureWatcher<Loaded>* m_watcher;

    QLineEdit* m_search;
    QLabel* m_count;
    QListWidget* m_list;
    QLabel* m_name;
    QLabel* m_details;
    ImageView* m_view;
    QPushButton* m_exportAll;
    QPushButton* m_exportSelected;
    QPushButton* m_extract;
    QPushButton* m_replace;
    QPushButton* m_import;
};

} // namespace viewer::app
