#include "TextureBrowser.hpp"

#include "Exporter.hpp"
#include "ImportExportDialogs.hpp"
#include "QtConvert.hpp"
#include "preview/ImageView.hpp"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <fstream>

namespace viewer::app {

namespace {

constexpr int kThumb = 64;

/// A thumbnail: the picture scaled into a square, on a checkerboard so
/// see-through textures read as such (colours from the palette).
QIcon thumbnail(const QImage& image, const QPalette& palette)
{
    QPixmap pixmap(kThumb, kThumb);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    const QImage scaled = image.scaled(kThumb, kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QRect area((kThumb - scaled.width()) / 2, (kThumb - scaled.height()) / 2, scaled.width(), scaled.height());
    constexpr int cell = 8;
    for (int y = area.top(); y < area.bottom(); y += cell)
        for (int x = area.left(); x < area.right(); x += cell)
            p.fillRect(QRect(x, y, cell, cell).intersected(area),
                       ((x - area.left()) / cell + (y - area.top()) / cell) % 2 ? palette.color(QPalette::Midlight)
                                                                                  : palette.color(QPalette::Mid));
    p.drawImage(area, scaled);
    return QIcon(pixmap);
}

/// "name" made safe as a file name.
QString fileNameFor(const std::string& name)
{
    QString s = toQString(name);
    for (QChar& c : s)
        if (QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    return s.isEmpty() ? QStringLiteral("texture") : s;
}

QString extensionFor(ImageExport format)
{
    switch (format) {
    case ImageExport::Png: return QStringLiteral("png");
    case ImageExport::Dds: return QStringLiteral("dds");
    case ImageExport::Jpg: return QStringLiteral("jpg");
    case ImageExport::Bmp: return QStringLiteral("bmp");
    }
    return QStringLiteral("png");
}

bool writeBytes(const QString& path, ByteSpan bytes)
{
    std::ofstream out(toPath(path), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace

TextureBrowser::TextureBrowser(QWidget* parent)
    : QWidget(parent, Qt::Window), m_watcher(new QFutureWatcher<Loaded>(this)), m_search(new QLineEdit(this)),
      m_count(new QLabel(this)), m_list(new QListWidget(this)), m_name(new QLabel(this)), m_details(new QLabel(this)),
      m_view(new ImageView(this)), m_exportAll(new QPushButton(tr("Export All…"), this)),
      m_exportSelected(new QPushButton(tr("Export Selected…"), this)), m_extract(new QPushButton(tr("Extract…"), this)),
      m_replace(new QPushButton(tr("Replace…"), this)), m_import(new QPushButton(tr("Import Folder…"), this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true); // pictures from Explorer (see dropEvent)
    connect(m_watcher, &QFutureWatcher<Loaded>::finished, this, &TextureBrowser::applyLoaded);

    m_search->setPlaceholderText(tr("Search textures"));
    m_search->setClearButtonEnabled(true);
    m_search->setAcceptDrops(false); // a dropped picture is for the window, not the search text
    connect(m_search, &QLineEdit::textChanged, this, &TextureBrowser::filter);
    m_count->setForegroundRole(QPalette::PlaceholderText);

    m_list->setIconSize(QSize(kThumb, kThumb));
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setUniformItemSizes(true);
    m_list->setSpacing(0);
    m_list->setToolTip(tr("Drag a picture from Explorer onto a texture to replace it"));
    connect(m_list, &QListWidget::currentRowChanged, this, &TextureBrowser::showCurrent);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &TextureBrowser::updateButtons);

    QFont big = m_name->font();
    big.setPointSizeF(big.pointSizeF() * 1.6);
    m_name->setFont(big);
    m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setForegroundRole(QPalette::PlaceholderText);

    auto* left = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_search);
    leftLayout->addWidget(m_list, 1);
    leftLayout->addWidget(m_count);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->addWidget(m_name);
    rightLayout->addWidget(m_details);
    rightLayout->addWidget(m_view, 1);

    auto* splitter = new QSplitter(this);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    QSettings settings;
    if (!splitter->restoreState(settings.value("textures/splitter").toByteArray()))
        splitter->setSizes({300, 700});
    connect(splitter, &QSplitter::splitterMoved, this,
            [splitter] { QSettings().setValue("textures/splitter", splitter->saveState()); });

    m_exportAll->setToolTip(tr("Save every texture as a picture (PNG, DDS, JPG or BMP)"));
    m_exportSelected->setToolTip(tr("Save the selected textures as pictures"));
    m_extract->setToolTip(tr("Save the selected textures exactly as the game stores them (.dds, every mip level)"));
    m_replace->setToolTip(tr("Replace the selected texture with a picture of your own. Needs Edit Mode."));
    m_import->setToolTip(tr("Replace the textures that have a picture named after them in a folder "
                            "(as Export All writes them). Needs Edit Mode."));
    connect(m_exportAll, &QPushButton::clicked, this, [this] {
        std::vector<int> all(m_loaded.entries.size());
        for (std::size_t i = 0; i < all.size(); ++i)
            all[i] = static_cast<int>(i);
        exportTextures(all);
    });
    connect(m_exportSelected, &QPushButton::clicked, this, [this] { exportTextures(selected()); });
    connect(m_extract, &QPushButton::clicked, this, [this] { extractTextures(selected()); });
    connect(m_replace, &QPushButton::clicked, this, [this] {
        if (const auto s = selected(); s.size() == 1)
            emit replaceRequested(s.front(), {});
    });
    connect(m_import, &QPushButton::clicked, this, [this] { emit importRequested({}); });
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_exportAll);
    buttons->addWidget(m_exportSelected);
    buttons->addWidget(m_extract);
    buttons->addSpacing(16);
    buttons->addWidget(m_replace);
    buttons->addWidget(m_import);
    buttons->addStretch(1);
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addLayout(buttons);

    if (!restoreGeometry(settings.value("textures/geometry").toByteArray()))
        resize(1100, 720);
    updateButtons();
}

TextureBrowser::~TextureBrowser()
{
    m_watcher->waitForFinished();
}

void TextureBrowser::closeEvent(QCloseEvent* event)
{
    if (!testAttribute(Qt::WA_DontShowOnScreen))
        QSettings().setValue("textures/geometry", saveGeometry());
    QWidget::closeEvent(event);
}

// ---- Drag & drop: pictures from Explorer replace textures --------------------------------

QStringList TextureBrowser::droppedPictures(const QMimeData* mime) const
{
    QStringList files;
    if (!mime || !mime->hasUrls() || m_loaded.entries.empty())
        return files;
    static const QStringList kinds{"png", "dds", "jpg", "jpeg", "bmp"};
    for (const QUrl& url : mime->urls()) {
        const QFileInfo info(url.toLocalFile());
        if (!url.isLocalFile() || !info.isFile() || !kinds.contains(info.suffix(), Qt::CaseInsensitive))
            return {}; // something else is in the drag: not ours
        files << info.filePath();
    }
    return files;
}

QListWidgetItem* TextureBrowser::rowAt(const QPoint& pos) const
{
    const QPoint p = m_list->viewport()->mapFrom(this, pos);
    if (!m_list->viewport()->rect().contains(p))
        return nullptr;
    QListWidgetItem* item = m_list->itemAt(p);
    return item && !item->isHidden() ? item : nullptr;
}

void TextureBrowser::dragEnterEvent(QDragEnterEvent* event)
{
    dragMoveEvent(event);
}

void TextureBrowser::dragMoveEvent(QDragMoveEvent* event)
{
    const QStringList files = droppedPictures(event->mimeData());
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    // One picture over a texture: that texture (shown, so you see what you'll replace).
    if (QListWidgetItem* item = files.size() == 1 ? rowAt(event->position().toPoint()) : nullptr) {
        if (m_list->currentItem() != item)
            m_list->setCurrentItem(item);
        m_count->setText(tr("Drop to replace %1").arg(item->toolTip()));
    } else {
        m_count->setText(tr("Drop to replace the textures named after the %n picture(s)", nullptr,
                            static_cast<int>(files.size())));
    }
}

void TextureBrowser::dragLeaveEvent(QDragLeaveEvent* event)
{
    filter(m_search->text()); // the count again
    QWidget::dragLeaveEvent(event);
}

void TextureBrowser::dropEvent(QDropEvent* event)
{
    const QStringList files = droppedPictures(event->mimeData());
    filter(m_search->text());
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    const QListWidgetItem* item = files.size() == 1 ? rowAt(event->position().toPoint()) : nullptr;
    const int index = item ? item->data(Qt::UserRole).toInt() : -1;
    // Act after the drop completes so Explorer isn't blocked by our dialogs.
    QTimer::singleShot(0, this, [this, index, files] {
        if (index >= 0)
            emit replaceRequested(index, files.front());
        else
            emit importRequested(files);
    });
}

void TextureBrowser::setSource(std::shared_ptr<Session> session, const VfsNode* node)
{
    m_session = std::move(session);
    m_node = node;
    setWindowTitle(tr("Textures — %1").arg(node ? toQString(node->name()) : QString()));
    m_list->clear();
    m_view->clear();
    m_name->setText(node ? tr("Loading…") : QString());
    m_details->clear();
    m_loaded = {};
    updateButtons();
    if (!m_session || !node)
        return;
    m_watcher->waitForFinished();
    m_watcher->setFuture(QtConcurrent::run([session = m_session, node]() -> Loaded {
        Loaded l;
        l.format = textureDictionary(*session, *node);
        auto bytes = session->vfs.read(*node);
        if (!l.format || !bytes) {
            l.error = bytes ? tr("This file holds no textures.") : toQString(bytes.error().message);
            return l;
        }
        l.data = std::move(*bytes);
        auto list = l.format->list(l.data);
        if (!list) {
            l.error = toQString(list.error().message);
            return l;
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            Entry e{(*list)[i], {}};
            if (auto s = l.format->surfaces(l.data, i)) {
                const ByteSpan top = ByteSpan(s->data).first(
                    std::min(s->data.size(), image::surfaceSize(s->format, s->width, s->height)));
                if (auto img = image::decodeSurface(top, s->format, s->width, s->height))
                    e.image = QImage(img->rgba.data(), static_cast<int>(img->width), static_cast<int>(img->height),
                                     static_cast<qsizetype>(img->width) * 4, QImage::Format_RGBA8888)
                                  .copy();
            }
            l.entries.push_back(std::move(e));
        }
        return l;
    }));
}

void TextureBrowser::applyLoaded()
{
    m_loaded = m_watcher->result();
    m_list->clear();
    if (!m_loaded.error.isEmpty()) {
        m_name->setText(m_loaded.error);
        updateButtons();
        return;
    }
    for (std::size_t i = 0; i < m_loaded.entries.size(); ++i) {
        const Entry& e = m_loaded.entries[i];
        auto* item = new QListWidgetItem(thumbnail(e.image, palette()),
                                         QStringLiteral("%1\n%2").arg(toQString(e.info.name), details(e.info)), m_list);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setToolTip(toQString(e.info.name));
        item->setSizeHint(QSize(0, kThumb + 8));
    }
    filter(m_search->text());
    if (m_list->count())
        m_list->setCurrentRow(0);
    updateButtons();
}

QString TextureBrowser::details(const DictionaryTexture& t) const
{
    return tr("%1×%2 · %3 · %n mip level(s)", nullptr, static_cast<int>(t.mipCount))
        .arg(t.width)
        .arg(t.height)
        .arg(toQString(image::pixelFormatName(t.format)));
}

void TextureBrowser::filter(const QString& text)
{
    int shown = 0;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        const bool match = text.isEmpty() || item->toolTip().contains(text, Qt::CaseInsensitive);
        item->setHidden(!match);
        shown += match;
    }
    m_count->setText(shown == m_list->count() ? tr("%n texture(s)", nullptr, shown)
                                              : tr("%1 of %n texture(s)", nullptr, m_list->count()).arg(shown));
}

void TextureBrowser::showCurrent()
{
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) {
        m_view->clear();
        m_name->clear();
        m_details->clear();
        return;
    }
    const Entry& e = m_loaded.entries[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())];
    m_name->setText(toQString(e.info.name));
    const std::size_t bytes = image::chainSize(e.info.format, e.info.width, e.info.height, e.info.mipCount);
    m_details->setText(tr("Width %1 · Height %2 · %3 · %n mip level(s) · %4 stored", nullptr,
                          static_cast<int>(e.info.mipCount))
                           .arg(e.info.width)
                           .arg(e.info.height)
                           .arg(toQString(image::pixelFormatName(e.info.format)))
                           .arg(QLocale().formattedDataSize(static_cast<qint64>(bytes))));
    m_details->setToolTip(pixelFormatDescription(e.info.format));
    m_view->setImage(e.image, QStringLiteral("%1 × %2  ·  %3")
                                  .arg(e.info.width)
                                  .arg(e.info.height)
                                  .arg(toQString(image::pixelFormatName(e.info.format))),
                     fileNameFor(e.info.name));
}

std::vector<int> TextureBrowser::selected() const
{
    std::vector<int> out;
    for (const QListWidgetItem* item : m_list->selectedItems())
        if (!item->isHidden())
            out.push_back(item->data(Qt::UserRole).toInt());
    std::ranges::sort(out);
    return out;
}

void TextureBrowser::setEditMode(bool on)
{
    m_editMode = on;
    updateButtons();
}

void TextureBrowser::updateButtons()
{
    const bool any = !m_loaded.entries.empty();
    const auto sel = selected();
    m_exportAll->setEnabled(any);
    m_exportSelected->setEnabled(!sel.empty());
    m_extract->setEnabled(!sel.empty());
    m_replace->setEnabled(m_editMode && sel.size() == 1 && m_node && Vfs::canReplace(*m_node));
    m_import->setEnabled(m_editMode && any && m_node && Vfs::canReplace(*m_node));
}

void TextureBrowser::exportTextures(const std::vector<int>& indices)
{
    if (indices.empty() || !m_loaded.format)
        return;
    QSettings settings;
    const QString lastDir = settings.value("paths/lastExtractDir").toString();
    struct Target {
        int index;
        QString path;
        ImageExport format;
    };
    std::vector<Target> targets;
    if (indices.size() == 1) {
        // One texture: a file name and the format from its extension.
        const Entry& e = m_loaded.entries[static_cast<std::size_t>(indices.front())];
        QString chosenFilter;
        const QString file = QFileDialog::getSaveFileName(
            this, tr("Export %1").arg(toQString(e.info.name)),
            QDir(lastDir).filePath(fileNameFor(e.info.name) + QStringLiteral(".png")),
            tr("PNG picture (*.png);;DDS texture (*.dds);;JPEG picture (*.jpg);;BMP picture (*.bmp)"), &chosenFilter);
        if (file.isEmpty())
            return;
        settings.setValue("paths/lastExtractDir", QFileInfo(file).absolutePath());
        const QString ext = QFileInfo(file).suffix().toLower();
        const ImageExport format = ext == "dds"                    ? ImageExport::Dds
                                   : ext == "jpg" || ext == "jpeg" ? ImageExport::Jpg
                                   : ext == "bmp"                  ? ImageExport::Bmp
                                                                   : ImageExport::Png;
        targets.push_back({indices.front(), file, format});
    } else {
        const auto format = chooseImageExport(this);
        if (!format)
            return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Export %n texture(s) to", nullptr,
                                                                       static_cast<int>(indices.size())),
                                                              lastDir);
        if (dir.isEmpty())
            return;
        settings.setValue("paths/lastExtractDir", dir);
        for (int i : indices)
            targets.push_back({i, QDir(dir).filePath(fileNameFor(m_loaded.entries[static_cast<std::size_t>(i)].info.name)
                                                     + QLatin1Char('.') + extensionFor(*format)),
                               *format});
    }

    QStringList failed;
    for (const Target& t : targets) {
        const Entry& e = m_loaded.entries[static_cast<std::size_t>(t.index)];
        bool ok = false;
        if (t.format == ImageExport::Dds) { // lossless: the stored pixels and mip levels
            auto s = m_loaded.format->surfaces(m_loaded.data, static_cast<std::size_t>(t.index));
            auto dds = s ? image::writeDds(*s) : Result<ByteBuffer>(std::unexpected(s.error()));
            ok = dds && writeBytes(t.path, *dds);
        } else {
            const QImage img = t.format == ImageExport::Jpg ? e.image.convertToFormat(QImage::Format_RGB888) : e.image;
            ok = img.save(t.path, t.format == ImageExport::Jpg ? "JPG" : t.format == ImageExport::Bmp ? "BMP" : "PNG");
        }
        if (!ok)
            failed << QFileInfo(t.path).fileName();
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Export"), tr("Couldn't write: %1").arg(failed.join(QStringLiteral(", "))));
}

void TextureBrowser::extractTextures(const std::vector<int>& indices)
{
    if (indices.empty() || !m_loaded.format)
        return;
    QSettings settings;
    const QString lastDir = settings.value("paths/lastExtractDir").toString();
    QString dir;
    QString single;
    if (indices.size() == 1) {
        single = QFileDialog::getSaveFileName(
            this, tr("Extract %1").arg(toQString(m_loaded.entries[static_cast<std::size_t>(indices.front())].info.name)),
            QDir(lastDir).filePath(fileNameFor(m_loaded.entries[static_cast<std::size_t>(indices.front())].info.name)
                                   + QStringLiteral(".dds")),
            tr("DDS texture (*.dds)"));
        if (single.isEmpty())
            return;
        settings.setValue("paths/lastExtractDir", QFileInfo(single).absolutePath());
    } else {
        dir = QFileDialog::getExistingDirectory(this, tr("Extract %n texture(s) to", nullptr,
                                                         static_cast<int>(indices.size())),
                                                lastDir);
        if (dir.isEmpty())
            return;
        settings.setValue("paths/lastExtractDir", dir);
    }
    QStringList failed;
    for (int i : indices) {
        const Entry& e = m_loaded.entries[static_cast<std::size_t>(i)];
        const QString path = single.isEmpty() ? QDir(dir).filePath(fileNameFor(e.info.name) + QStringLiteral(".dds")) : single;
        auto s = m_loaded.format->surfaces(m_loaded.data, static_cast<std::size_t>(i));
        auto dds = s ? image::writeDds(*s) : Result<ByteBuffer>(std::unexpected(s.error()));
        if (!dds || !writeBytes(path, *dds))
            failed << QFileInfo(path).fileName();
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Extract"), tr("Couldn't write: %1").arg(failed.join(QStringLiteral(", "))));
}

} // namespace viewer::app
