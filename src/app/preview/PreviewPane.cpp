#include "PreviewPane.hpp"

#include "../BackupBanner.hpp"
#include "../BusyOverlay.hpp"
#include "../QtConvert.hpp"
#include "../SettingsDialog.hpp"
#include "AnimationSupport.hpp"
#include "HexView.hpp"
#include "ImageView.hpp"
#include "FontView.hpp"
#include "ModelView.hpp"
#include "SoundBankView.hpp"
#include "TextView.hpp"
#include "VideoView.hpp"

#include "viewer/core/Text.hpp"

#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <optional>

namespace viewer::app {

/// Everything the background loader produces for one file.
struct PreviewResult {
    quint64 generation = 0;
    QByteArray bytes;
    bool truncated = false;
    QString error;

    QImage image;
    QString imageInfo;
    QString imageError;
    std::shared_ptr<const image::Surfaces> surfaces; ///< A texture's stored mip levels / layers.

    std::optional<StructureNode> structure;
    QString structureError;

    QString text;
    bool hasText = false;
    TextSyntax syntax = TextSyntax::Plain;
    QString textInfo; ///< "UTF-16 LE · CRLF"
    bool decompiled = false;   ///< Text made from a binary file…
    bool recompilable = false; ///< …whose edited values can be written back (Edit).
    bool textTruncated = false;
    qint64 textLimit = 0; ///< Settings: text shown up to this many bytes.

    std::shared_ptr<const Mesh> mesh;
    QString meshError;
    SkeletonMatch skeleton; ///< The skeleton (and own animation) the model is posed with.

    std::shared_ptr<const Font> font;
    QString fontError;

    std::shared_ptr<const SoundBank> bank;
    bool bankRebuildable = false; ///< Its sounds can be replaced (not a plain .wav).
    QString bankError;

    bool isVideo = false;
    QString videoFile;     ///< On disk: streamed from there.
    QByteArray videoData;  ///< Packed in an archive: played from memory.
};

namespace {

constexpr std::uint64_t kHexOnlyLimit = 1ull << 20; // hex dump of audio banks / videos (the rest isn't needed)

QString exactSize(std::uint64_t bytes)
{
    const QLocale locale;
    return QStringLiteral("%1 bytes (%2)")
        .arg(locale.toString(static_cast<qulonglong>(bytes)),
             locale.formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat));
}

/// All the textures of a texture pack side by side (largest level, scaled to a
/// thumbnail), each with its name under it — the preview of a .bpr.
QImage textureSheet(const TextureDictionaryFormat& format, ByteSpan data, std::size_t& count, QString& error)
{
    auto list = format.list(data);
    if (!list) {
        error = toQString(list.error().message);
        return {};
    }
    count = list->size();
    if (list->empty())
        return {};
    constexpr int kThumb = 160, kLabel = 22, kGap = 8;
    // Roughly square overall (a little wider than tall), at most 16 across.
    const int columns = std::clamp(static_cast<int>(std::ceil(std::sqrt(static_cast<double>(list->size()) * 1.5))), 1,
                                   std::min<int>(16, static_cast<int>(list->size())));
    const int rows = (static_cast<int>(list->size()) + columns - 1) / columns;
    QImage sheet(columns * (kThumb + kGap) + kGap, rows * (kThumb + kLabel + kGap) + kGap, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::transparent);
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QFont font = painter.font();
    font.setPixelSize(12);
    painter.setFont(font);
    painter.setPen(QColor(0x8a, 0x8a, 0x8a)); // readable on light and dark backgrounds
    for (std::size_t i = 0; i < list->size(); ++i) {
        const QRect cell(kGap + static_cast<int>(i % columns) * (kThumb + kGap),
                         kGap + static_cast<int>(i / columns) * (kThumb + kLabel + kGap), kThumb, kThumb);
        if (auto s = format.surfaces(data, i)) {
            const std::size_t top = image::surfaceSize(s->format, s->width, s->height);
            if (auto img = image::decodeSurface(ByteSpan(s->data).first(std::min(top, s->data.size())), s->format,
                                                s->width, s->height)) {
                const QImage view(img->rgba.data(), static_cast<int>(img->width), static_cast<int>(img->height),
                                  static_cast<qsizetype>(img->width) * 4, QImage::Format_RGBA8888);
                const QImage thumb = view.scaled(kThumb, kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter.drawImage(cell.center() - QPoint(thumb.width() / 2, thumb.height() / 2), thumb);
            }
        }
        const QRect label(cell.left(), cell.bottom() + 2, kThumb, kLabel);
        painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop,
                         painter.fontMetrics().elidedText(toQString((*list)[i].name), Qt::ElideMiddle, kThumb));
    }
    return sheet;
}

/// A vector drawing as a picture (`pixels` across), on a transparent background.
QImage renderDrawing(const Drawing& drawing, int pixels)
{
    const double scale = pixels / std::max(drawing.width, drawing.height);
    QImage image(std::max(1, static_cast<int>(drawing.width * scale)), std::max(1, static_cast<int>(drawing.height * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.scale(scale, scale);
    for (const Drawing::Shape& shape : drawing.shapes) {
        // One path per shape so the triangles meet without seams. Fills are
        // fans over their outline: overlaps cancel out (even-odd), which leaves
        // the holes and hollows. Outlines are strips: their overlaps add up.
        QPainterPath path;
        path.setFillRule(shape.outline ? Qt::WindingFill : Qt::OddEvenFill);
        for (std::size_t i = 0; i + 2 < shape.triangles.size(); i += 3) {
            const auto& a = shape.triangles[i];
            const auto& b = shape.triangles[i + 1];
            const auto& c = shape.triangles[i + 2];
            // Strip triangles alternate their winding: make each one count the same.
            const bool turn = shape.outline && (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) < 0;
            path.addPolygon(QPolygonF{QPointF(a[0], a[1]), turn ? QPointF(c[0], c[1]) : QPointF(b[0], b[1]),
                                      turn ? QPointF(b[0], b[1]) : QPointF(c[0], c[1])});
            path.closeSubpath();
        }
        painter.setBrush(QColor::fromRgbF(shape.color[0], shape.color[1], shape.color[2], shape.color[3]));
        painter.drawPath(path);
    }
    // Names under the items of a sheet.
    if (!drawing.labels.empty()) {
        painter.resetTransform();
        QFont font = painter.font();
        font.setPixelSize(std::max(12, static_cast<int>(96 * scale)));
        painter.setFont(font);
        painter.setPen(QColor(0x8a, 0x8a, 0x8a)); // readable on light and dark backgrounds
        for (const Drawing::Label& label : drawing.labels) {
            const QRectF box(label.x * scale, label.y * scale, label.width * scale, font.pixelSize() * 1.6);
            painter.drawText(box, Qt::AlignHCenter | Qt::AlignTop,
                             painter.fontMetrics().elidedText(toQString(label.text), Qt::ElideMiddle,
                                                              static_cast<int>(box.width())));
        }
    }
    return image;
}

/// Decodes up to r.textLimit bytes of text and notes its encoding and line endings.
void decodeTextInto(PreviewResult& r, ByteSpan data, const std::string& name)
{
    // Cut long files at an even length so UTF-16 isn't split mid-character.
    const std::size_t limit = static_cast<std::size_t>(r.textLimit) & ~std::size_t{1};
    r.textTruncated = data.size() > limit;
    auto doc = decodeText(data.first(std::min(data.size(), limit)));
    if (!doc) { // e.g. a UTF-8 BOM followed by invalid bytes
        r.text = QString::fromLatin1(reinterpret_cast<const char*>(data.data()),
                                     static_cast<qsizetype>(std::min(data.size(), limit)));
        r.textInfo = QObject::tr("Unreadable text encoding (%1)").arg(toQString(doc.error().message));
    } else {
        r.text = toQString(doc->text);
        r.textInfo = QStringLiteral("%1  ·  %2").arg(toQString(encodingName(doc->encoding)),
                                                     doc->lineEnding == LineEnding::CRLF ? QStringLiteral("CRLF")
                                                                                          : QStringLiteral("LF"));
    }
    r.syntax = syntaxFor(name, doc ? std::string_view(doc->text) : std::string_view{});
    r.hasText = true;
}

/// `limits`: the preview sizes from Settings (read on the UI thread).
std::shared_ptr<PreviewResult> loadPreview(std::shared_ptr<Session> session, const VfsNode* node, quint64 generation,
                                           AppSettings limits)
{
    auto r = std::make_shared<PreviewResult>();
    r->textLimit = static_cast<qint64>(limits.textLimit());
    r->generation = generation;
    const std::string ext = extensionOf(node->name());
    const FileTypeInfo type = session->content.fileTypes().lookup(node->name());

    // Audio banks and videos can be hundreds of MB: read only what's needed.
    if (type.category == FileCategory::Audio || type.category == FileCategory::Video) {
        auto source = Vfs::source(*node);
        if (!source) {
            r->error = toQString(source.error().message);
            return r;
        }
        const auto head = (*source)->read(0, static_cast<std::size_t>(std::min((*source)->size(), kHexOnlyLimit)));
        if (!head) {
            r->error = toQString(head.error().message);
            return r;
        }
        r->bytes = QByteArray(reinterpret_cast<const char*>(head->data()), static_cast<qsizetype>(head->size()));
        r->truncated = (*source)->size() > head->size();
        const ByteSpan header = ByteSpan(*head).first(std::min<std::size_t>(head->size(), 4096));
        if (type.category == FileCategory::Audio) {
            if (const SoundBankFormat* format = session->content.findSoundBankFormat(ext, header)) {
                r->bankRebuildable = format->canRebuild() && ext != "wav";
                if (auto bank = format->open(*source))
                    r->bank = std::shared_ptr<const SoundBank>(std::move(*bank));
                else
                    r->bankError = toQString(bank.error().message);
            } else {
                r->bankError = QObject::tr("This audio format isn't supported yet.");
            }
        } else {
            r->isVideo = true;
            if (!node->diskPath().empty()) {
                r->videoFile = toQString(node->diskPath());
            } else if ((*source)->size() <= limits.packedPreviewLimit()) {
                if (auto all = (*source)->read(0, static_cast<std::size_t>((*source)->size())))
                    r->videoData = QByteArray(reinterpret_cast<const char*>(all->data()),
                                              static_cast<qsizetype>(all->size()));
            }
        }
        return r;
    }

    if (node->archive() && node->size() > limits.packedPreviewLimit()) {
        r->error = QObject::tr("This file is too large to preview (%1). Extract it instead.")
                       .arg(QLocale().formattedDataSize(static_cast<qint64>(node->size())));
        return r;
    }
    auto data = session->vfs.read(*node, limits.looseReadLimit());
    if (!data) {
        r->error = toQString(data.error().message);
        return r;
    }
    r->truncated = !node->archive() && node->size() > data->size();
    const ByteSpan span(*data);

    // Image
    if (const ImageDecoder* decoder = session->content.findImageDecoder(ext, span)) {
        if (auto img = decoder->decode(span)) {
            const QImage view(img->rgba.data(), static_cast<int>(img->width), static_cast<int>(img->height),
                              static_cast<qsizetype>(img->width) * 4, QImage::Format_RGBA8888);
            r->image = view.copy(); // detach from the decoder's buffer
            r->imageInfo = QStringLiteral("%1 × %2  ·  %3  ·  %4")
                               .arg(img->width)
                               .arg(img->height)
                               .arg(toQString(img->format))
                               .arg(QObject::tr("%n mip(s)", nullptr, static_cast<int>(img->mipCount)));
            if (img->layers > 1)
                r->imageInfo += QObject::tr("  ·  %n layer(s)", nullptr, static_cast<int>(img->layers));
            // The other levels, for the mip and face pickers (decoded only when picked).
            if (img->mipCount > 1 || img->layers > 1)
                if (auto surfaces = decoder->surfaces(span))
                    r->surfaces = std::make_shared<const image::Surfaces>(std::move(*surfaces));
        } else {
            r->imageError = toQString(img.error().message);
        }
    } else if (const DrawingDecoder* layout = session->content.findDrawingDecoder(ext, span)) {
        // A vector drawing, drawn as a picture.
        if (auto drawing = layout->decode(span)) {
            r->image = renderDrawing(*drawing, drawing->labels.empty() ? 2048 : 4096);
            r->imageInfo = QObject::tr("Vector drawing · %n shape(s)", nullptr, static_cast<int>(drawing->shapes.size()));
            for (const std::string& note : drawing->notes)
                r->imageInfo += QStringLiteral("  ·  ") + toQString(note);
        } else {
            r->imageError = toQString(drawing.error().message);
        }
    } else if (const TextureDictionaryFormat* pack = session->content.findTextureDictionary(ext, span);
               pack && !session->content.findMeshDecoder(ext, span)) {
        // A pack of textures (a model's are shown with the model instead).
        std::size_t count = 0;
        r->image = textureSheet(*pack, span, count, r->imageError);
        r->imageInfo = QObject::tr("%n texture(s) — Textures… (toolbar) to browse, export or replace them", nullptr,
                                   static_cast<int>(count));
    } else if (type.category == FileCategory::Texture) {
        QImage img; // formats Qt reads natively (PNG, JPEG, BMP, GIF...)
        if (img.loadFromData(reinterpret_cast<const uchar*>(data->data()), static_cast<int>(data->size()))) {
            r->image = img;
            r->imageInfo = QStringLiteral("%1 × %2").arg(img.width()).arg(img.height());
        }
    }

    // Structure
    if (const StructureParser* parser = session->content.findStructureParser(ext, span)) {
        if (auto s = parser->parse(span))
            r->structure = std::move(*s);
        else
            r->structureError = toQString(s.error().message);
    }

    // 3D model
    if (const MeshDecoder* decoder = session->content.findMeshDecoder(ext, span)) {
        if (auto mesh = decoder->decodeWith(span, session->vfs.decompileContext(*node))) {
            r->mesh = std::make_shared<const Mesh>(std::move(*mesh));
            r->skeleton = findSkeleton(*session, *node, *r->mesh);
        } else
            r->meshError = toQString(mesh.error().message);
    }

    // Font
    if (const FontDecoder* decoder = session->content.findFontDecoder(ext, span)) {
        if (auto font = decoder->decode(span))
            r->font = std::make_shared<const Font>(std::move(*font));
        else
            r->fontError = toQString(font.error().message);
    }

    r->bytes = QByteArray(reinterpret_cast<const char*>(data->data()), static_cast<qsizetype>(data->size()));

    // Text: a decompiler's reading of a binary file, or the file itself
    if (const TextDecompiler* decompiler = session->content.findDecompiler(ext, span)) {
        if (auto text = decompiler->decompileWith(span, session->vfs.decompileContext(*node))) {
            r->text = toQString(*text);
            r->hasText = true;
            r->decompiled = true;
            r->recompilable = decompiler->canRecompile();
            r->syntax = decompiler->syntax();
            r->textInfo = r->recompilable ? QObject::tr("%1, decompiled (Edit to change values)").arg(toQString(decompiler->name()))
                                          : QObject::tr("%1, decompiled (read-only)").arg(toQString(decompiler->name()));
        } else {
            r->structureError = toQString(text.error().message);
        }
    } else if (r->image.isNull() && (type.category == FileCategory::Text || looksLikeText(span))) {
        decodeTextInto(*r, span, node->name());
    }
    return r;
}

void addStructureItems(QTreeWidgetItem* parent, QTreeWidget* tree, const StructureNode& node, int depth)
{
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, toQString(node.label));
    item->setText(1, QStringLiteral("0x%1").arg(node.offset, 8, 16, QLatin1Char('0')).toUpper().replace("0X", "0x"));
    item->setText(2, QLocale().toString(static_cast<qulonglong>(node.size)));
    item->setText(3, toQString(node.detail));
    item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(node.offset));
    item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(node.size));
    item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    for (const auto& child : node.children)
        addStructureItems(item, tree, child, depth + 1);
    if (depth < 2)
        item->setExpanded(true);
}

} // namespace

PreviewPane::PreviewPane(QWidget* parent)
    : QWidget(parent), m_watcher(new QFutureWatcher<std::shared_ptr<PreviewResult>>(this)),
      m_title(new QLabel(this)), m_subtitle(new QLabel(this)),
      m_banner(new BackupBanner(this)), m_tabs(new QTabWidget(this)),
      m_previewStack(new QStackedWidget(this)), m_message(new QLabel(this)), m_image(new ImageView(this)),
      m_model(new ModelView(this)), m_fontView(new FontView(this)), m_bankView(new SoundBankView(this)), m_video(new VideoView(this)),
      m_text(new TextView(this)), m_structure(new QTreeWidget(this)),
      m_hexPanel(new HexPanel(this)), m_hex(m_hexPanel->view()),
      m_properties(new QTreeWidget(this))
{
    // Header
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    m_subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // Preview tab
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setForegroundRole(QPalette::PlaceholderText);
    // Previews are read-only; "Edit" opens the file in a TextEditorWindow.
    m_text->setEditButtonVisible(true);
    connect(m_text, &TextView::editRequested, this, &PreviewPane::editRequested);
    connect(m_bankView, &SoundBankView::replaceSoundRequested, this, &PreviewPane::replaceSoundRequested);
    connect(m_model, &ModelView::animationRequested, this, &PreviewPane::chooseAnimation);
    m_previewStack->addWidget(m_message);
    m_previewStack->addWidget(m_image);
    m_previewStack->addWidget(m_text);
    m_previewStack->addWidget(m_model);
    m_previewStack->addWidget(m_fontView);
    m_previewStack->addWidget(m_bankView);
    m_previewStack->addWidget(m_video);

    // Structure tab
    m_structure->setHeaderLabels({tr("Chunk"), tr("Offset"), tr("Size"), tr("Details")});
    m_structure->setUniformRowHeights(true);
    m_structure->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(m_structure, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item)
            m_hex->highlight(static_cast<qint64>(item->data(0, Qt::UserRole).toULongLong()),
                             static_cast<qint64>(item->data(0, Qt::UserRole + 1).toULongLong()));
    });
    connect(m_structure, &QTreeWidget::itemDoubleClicked, this, [this] { m_tabs->setCurrentIndex(HexTab); });

    // Properties tab
    m_properties->setHeaderLabels({tr("Property"), tr("Value")});
    m_properties->setRootIsDecorated(false);
    m_properties->setUniformRowHeights(true);
    m_properties->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    m_tabs->setDocumentMode(true);
    m_tabs->addTab(m_previewStack, tr("Preview"));
    m_tabs->addTab(m_structure, tr("Structure"));
    m_tabs->addTab(m_hexPanel, tr("Hex"));
    m_tabs->addTab(m_properties, tr("Properties"));
    connect(m_tabs->tabBar(), &QTabBar::tabBarClicked, this, [this](int index) {
        if (index >= 0)
            m_preferredTab = index;
    });

    connect(m_banner, &BackupBanner::backupRequested, this, &PreviewPane::backupRequested);

    auto* header = new QVBoxLayout;
    header->setContentsMargins(12, 10, 12, 6);
    header->setSpacing(2);
    header->addWidget(m_title);
    header->addWidget(m_subtitle);
    header->addSpacing(6);
    header->addWidget(m_banner);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_tabs, 1);
    m_busy = new BusyOverlay(m_tabs);

    connect(m_watcher, &QFutureWatcher<std::shared_ptr<PreviewResult>>::finished, this, [this] {
        if (!m_watcher->future().isValid() || m_watcher->future().resultCount() == 0)
            return;
        const auto result = m_watcher->result();
        if (result && result->generation == m_generation) {
            m_busy->stop();
            applyResult(*result);
        }
    });

    showNode(nullptr);
}

PreviewPane::~PreviewPane()
{
    m_watcher->waitForFinished();
}

void PreviewPane::setSession(std::shared_ptr<Session> session)
{
    // Let an in-flight load finish so the old session's files are released
    // before callers modify or restore them.
    m_watcher->waitForFinished();
    m_session = std::move(session);
    showNode(nullptr);
}

void PreviewPane::showMessage(const QString& message)
{
    m_message->setText(message);
    m_previewStack->setCurrentWidget(m_message);
}

void PreviewPane::showNode(const VfsNode* node)
{
    display(node, -1);
}

void PreviewPane::display(const VfsNode* node, int forcedTab)
{
    m_node = node;
    m_forcedTab = forcedTab;
    ++m_generation;
    m_busy->stop();
    m_image->clear();
    m_text->clear();
    m_model->clear();
    m_fontView->clear();
    m_bankView->clear();
    m_video->clear();
    m_structure->clear();
    m_hex->clear();

    if (!node || !m_session) {
        m_title->setText(m_session ? m_session->title() : tr("Nothing selected"));
        m_subtitle->setText(m_session ? tr("Select a file to preview it") : tr("Open a game to get started"));
        showMessage(tr("Double-click a file in the list to preview it."));
        fillProperties(nullptr, nullptr);
        return;
    }

    const FileTypeInfo type = m_session->content.fileTypes().lookup(node->name());
    m_title->setText(toQString(node->name()));

    if (node->isContainer()) {
        const QString kind = node->kind() == VfsNode::Kind::Archive
                                 ? toQString(node->archive() ? node->archive()->formatName() : "Archive")
                                 : tr("Folder");
        m_subtitle->setText(QStringLiteral("%1  ·  %2").arg(kind, toQString(node->virtualPath())));
        showMessage(tr("%n file(s), %1 unpacked", nullptr, static_cast<int>(node->fileCount()))
                        .arg(QLocale().formattedDataSize(static_cast<qint64>(node->size()))));
        fillProperties(node, nullptr);
        if (m_forcedTab >= 0)
            m_tabs->setCurrentIndex(m_forcedTab);
        emit previewReady();
        return;
    }

    m_subtitle->setText(QStringLiteral("%1  ·  %2").arg(toQString(type.description), toQString(node->virtualPath())));
    showMessage(tr("Loading…"));
    m_busy->start(tr("Loading %1…").arg(toQString(node->name())));
    fillProperties(node, nullptr);
    m_watcher->setFuture(QtConcurrent::run(loadPreview, m_session, node, m_generation, AppSettings::load()));
}

void PreviewPane::applyResult(const PreviewResult& result)
{
    m_hex->setData(result.bytes);

    if (result.structure)
        addStructureItems(nullptr, m_structure, *result.structure, 0);
    else if (!result.structureError.isEmpty())
        new QTreeWidgetItem(m_structure, {tr("Could not parse structure"), {}, {}, result.structureError});

    bool hasPreview = true;
    if (!result.error.isEmpty()) {
        showMessage(result.error);
        hasPreview = false;
    } else if (result.bank) {
        m_bankView->setBank(result.bank, m_session);
        m_bankView->setReplaceable(result.bankRebuildable && m_node && Vfs::canReplace(*m_node));
        m_previewStack->setCurrentWidget(m_bankView);
    } else if (result.isVideo && (!result.videoFile.isEmpty() || !result.videoData.isEmpty())) {
        if (!result.videoFile.isEmpty())
            m_video->setFile(result.videoFile);
        else
            m_video->setData(result.videoData, m_node ? toQString(m_node->name()) : QString());
        m_previewStack->setCurrentWidget(m_video);
    } else if (result.mesh) {
        m_model->setMesh(result.mesh);
        m_pickerState = {}; // a new model: the picker starts afresh (at the model's own animation, if any)
        m_pickerState.startFile = result.skeleton.animationSource;
        m_model->setSkeleton(result.skeleton.skeleton, result.skeleton.source);
        if (result.skeleton.animation)
            m_model->setAnimation(result.skeleton.animation, result.skeleton.animationSource);
        m_previewStack->setCurrentWidget(m_model);
    } else if (result.font) {
        m_fontView->setTypeface(result.font);
        m_previewStack->setCurrentWidget(m_fontView);
    } else if (!result.image.isNull()) {
        m_image->setImage(result.image, result.imageInfo, m_title->text());
        m_image->setSurfaces(result.surfaces);
        m_previewStack->setCurrentWidget(m_image);
    } else if (result.hasText) {
        const int lines = static_cast<int>(result.text.count(QLatin1Char('\n')) + 1);
        QString info = result.textInfo + tr("  ·  %n line(s)", nullptr, lines);
        if (result.textTruncated)
            info += tr("  ·  showing the first %1").arg(QLocale().formattedDataSize(result.textLimit));
        m_text->setText(result.text, result.syntax, info);
        m_text->setEditButtonVisible(!result.decompiled || result.recompilable);
        m_previewStack->setCurrentWidget(m_text);
    } else {
        hasPreview = false;
        if (!result.bankError.isEmpty())
            showMessage(tr("Could not read the audio: %1").arg(result.bankError));
        else if (!result.meshError.isEmpty())
            showMessage(tr("Could not read the model: %1").arg(result.meshError));
        else if (!result.fontError.isEmpty())
            showMessage(tr("Could not read the font: %1").arg(result.fontError));
        else if (result.isVideo)
            showMessage(tr("This video is too large to play from inside an archive. Extract it instead."));
        else
            showMessage(result.imageError.isEmpty()
                            ? tr("No visual preview for this file type yet.\nSee the Structure and Hex tabs.")
                            : tr("Could not decode image: %1").arg(result.imageError));
    }

    // Respect the tab the user last chose, unless it has nothing to show.
    int tab = m_forcedTab >= 0 ? m_forcedTab : m_preferredTab;
    if (tab == StructureTab && !result.structure)
        tab = PreviewTab;
    if (tab == PreviewTab && !hasPreview)
        tab = result.structure ? StructureTab : HexTab;
    m_tabs->setCurrentIndex(tab);

    fillProperties(m_node, &result);
    emit previewReady();
}

void PreviewPane::setBackupWarning(const QString& fileName)
{
    m_banner->setWarning(fileName);
}

void PreviewPane::setBackupInProgress(bool running)
{
    m_banner->setInProgress(running);
}

void PreviewPane::showProperties(const VfsNode* node)
{
    display(node, PropertiesTab);
}

void PreviewPane::chooseAnimation()
{
    if (!m_session || !m_model->skeleton())
        return;
    AnimationPicker picker(m_session, m_model->skeleton(), &m_pickerState, this);
    if (picker.exec() == QDialog::Accepted && picker.selectedNode())
        m_model->setAnimation(picker.selectedAnimation(), toQString(picker.selectedNode()->virtualPath()));
}

void PreviewPane::setCamera(const QString& spec)
{
    const QStringList v = spec.split(QLatin1Char(','));
    if (v.size() < 3)
        return;
    std::optional<QVector3D> target;
    if (v.size() >= 6)
        target = QVector3D(v[3].toFloat(), v[4].toFloat(), v[5].toFloat());
    m_model->setCamera(v[0].toFloat(), v[1].toFloat(), v[2].toFloat(), target);
}

bool PreviewPane::showAnimation(const QString& virtualPath, float frame)
{
    if (!m_session || !m_model->skeleton())
        return false;
    auto animation = loadAnimationAt(*m_session, virtualPath);
    if (!animation || !anim::fits(*m_model->skeleton(), *animation))
        return false;
    m_model->setAnimation(std::move(animation), virtualPath);
    m_model->showAnimationFrame(frame);
    return true;
}

void PreviewPane::setCurrentTab(int tab)
{
    m_preferredTab = tab;
    m_tabs->setCurrentIndex(tab);
}

void PreviewPane::fillProperties(const VfsNode* node, const PreviewResult* result)
{
    m_properties->clear();
    if (!node || !m_session)
        return;

    QFont bold = m_properties->font();
    bold.setWeight(QFont::DemiBold);
    QTreeWidgetItem* group = nullptr;
    auto section = [&](const QString& title) {
        group = new QTreeWidgetItem(m_properties, {title});
        group->setFont(0, bold);
        group->setFirstColumnSpanned(true);
        group->setFlags(Qt::ItemIsEnabled);
    };
    auto add = [&](const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(m_properties, {QStringLiteral("    ") + name, value});
        item->setToolTip(1, value);
    };

    const FileTypeInfo type = m_session->content.fileTypes().lookup(node->name());
    section(node->isFile() ? tr("File") : tr("Container"));
    add(tr("Name"), toQString(node->name()));
    add(tr("Location"), toQString(node->virtualPath()));
    if (!node->diskPath().empty())
        add(tr("On disk"), QDir::toNativeSeparators(toQString(node->diskPath())));
    if (node->isFile())
        add(tr("Type"), QStringLiteral("%1 (%2)").arg(toQString(type.description), toQString(categoryName(type.category))));
    else
        add(tr("Files"), QLocale().toString(static_cast<qulonglong>(node->fileCount())));
    add(node->isFile() ? tr("Size") : tr("Unpacked size"), exactSize(node->size()));
    if (node->archive() || node->kind() == VfsNode::Kind::Archive) {
        add(tr("Packed size"), exactSize(node->packedSize()));
        if (node->size() > 0)
            add(tr("Ratio"), QStringLiteral("%1%").arg(100.0 * static_cast<double>(node->packedSize())
                                                           / static_cast<double>(node->size()),
                                                       0, 'f', 1));
    }
    if (const auto& t = node->modified()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t->time_since_epoch()).count();
        add(tr("Modified"), QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::LongFormat));
    }

    if (node->kind() == VfsNode::Kind::Archive && node->archive()) {
        section(tr("Archive"));
        for (const auto& p : node->archive()->describe())
            add(toQString(p.name), toQString(p.value));
    } else if (node->isFile() && node->archive()) {
        section(tr("Archive entry"));
        for (const auto& p : node->archive()->describeEntry(node->entryIndex()))
            add(toQString(p.name), toQString(p.value));
    }

    if (result && !result->image.isNull()) {
        section(tr("Image"));
        add(tr("Details"), result->imageInfo);
    }
    if (result && result->mesh) {
        const QLocale locale;
        section(tr("Model"));
        add(tr("Vertices"), locale.toString(static_cast<qulonglong>(result->mesh->vertexCount())));
        add(tr("Triangles"), locale.toString(static_cast<qulonglong>(result->mesh->triangleCount())));
        add(tr("Parts"), locale.toString(static_cast<qulonglong>(result->mesh->parts.size())));
        add(tr("Detail levels"), locale.toString(static_cast<qulonglong>(result->mesh->lodCount())));
        for (const auto& note : result->mesh->notes)
            add(tr("Note"), toQString(note));
    }
    if (result && result->bank) {
        section(tr("Audio"));
        add(tr("Format"), toQString(result->bank->formatName()));
        for (const auto& p : result->bank->describe())
            add(toQString(p.name), toQString(p.value));
    }
    if (result && result->truncated) {
        section(tr("Note"));
        add(tr("Preview"), tr("Only the first %1 are shown").arg(QLocale().formattedDataSize(result->bytes.size())));
    }
}

} // namespace viewer::app
