#include "PreviewPane.hpp"

#include "../BackupBanner.hpp"
#include "../QtConvert.hpp"
#include "HexView.hpp"
#include "ImageView.hpp"
#include "ModelView.hpp"
#include "SoundBankView.hpp"
#include "TextView.hpp"
#include "VideoView.hpp"

#include "viewer/core/Text.hpp"

#include <QDateTime>
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

    std::optional<StructureNode> structure;
    QString structureError;

    QString text;
    bool hasText = false;
    TextSyntax syntax = TextSyntax::Plain;
    QString textInfo; ///< "UTF-16 LE · CRLF"
    bool decompiled = false; ///< Text made from a binary file: can't be edited as text.
    bool textTruncated = false;

    std::shared_ptr<const Mesh> mesh;
    QString meshError;

    std::shared_ptr<const SoundBank> bank;
    bool bankRebuildable = false; ///< Its sounds can be replaced (not a plain .wav).
    QString bankError;

    bool isVideo = false;
    QString videoFile;     ///< On disk: streamed from there.
    QByteArray videoData;  ///< Packed in an archive: played from memory.
};

namespace {

constexpr std::uint64_t kLooseReadLimit = 64ull << 20;    // hex/preview of big loose files
constexpr std::uint64_t kPackedPreviewLimit = 512ull << 20; // packed entries must be read whole
constexpr qsizetype kTextLimit = 4 << 20;
constexpr std::uint64_t kHexOnlyLimit = 1ull << 20; // hex dump of audio banks / videos (the rest isn't needed)

QString exactSize(std::uint64_t bytes)
{
    const QLocale locale;
    return QStringLiteral("%1 bytes (%2)")
        .arg(locale.toString(static_cast<qulonglong>(bytes)),
             locale.formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat));
}

/// Decodes up to kTextLimit bytes of text and notes its encoding and line endings.
void decodeTextInto(PreviewResult& r, ByteSpan data, const std::string& name)
{
    // Cut long files at an even length so UTF-16 isn't split mid-character.
    const std::size_t limit = static_cast<std::size_t>(kTextLimit) & ~std::size_t{1};
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

std::shared_ptr<PreviewResult> loadPreview(std::shared_ptr<Session> session, const VfsNode* node, quint64 generation)
{
    auto r = std::make_shared<PreviewResult>();
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
            } else if ((*source)->size() <= kPackedPreviewLimit) {
                if (auto all = (*source)->read(0, static_cast<std::size_t>((*source)->size())))
                    r->videoData = QByteArray(reinterpret_cast<const char*>(all->data()),
                                              static_cast<qsizetype>(all->size()));
            }
        }
        return r;
    }

    if (node->archive() && node->size() > kPackedPreviewLimit) {
        r->error = QObject::tr("This file is too large to preview (%1). Extract it instead.")
                       .arg(QLocale().formattedDataSize(static_cast<qint64>(node->size())));
        return r;
    }
    auto data = session->vfs.read(*node, kLooseReadLimit);
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
        } else {
            r->imageError = toQString(img.error().message);
        }
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
        if (auto mesh = decoder->decode(span))
            r->mesh = std::make_shared<const Mesh>(std::move(*mesh));
        else
            r->meshError = toQString(mesh.error().message);
    }

    r->bytes = QByteArray(reinterpret_cast<const char*>(data->data()), static_cast<qsizetype>(data->size()));

    // Text: a decompiler's reading of a binary file, or the file itself
    if (const TextDecompiler* decompiler = session->content.findDecompiler(ext, span)) {
        if (auto text = decompiler->decompile(span)) {
            r->text = toQString(*text);
            r->hasText = true;
            r->decompiled = true;
            r->syntax = decompiler->syntax();
            r->textInfo = QObject::tr("%1, decompiled (read-only)").arg(toQString(decompiler->name()));
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
      m_model(new ModelView(this)), m_bankView(new SoundBankView(this)), m_video(new VideoView(this)),
      m_text(new TextView(this)), m_structure(new QTreeWidget(this)), m_hex(new HexView(this)),
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
    m_previewStack->addWidget(m_message);
    m_previewStack->addWidget(m_image);
    m_previewStack->addWidget(m_text);
    m_previewStack->addWidget(m_model);
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
    m_tabs->addTab(m_hex, tr("Hex"));
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

    connect(m_watcher, &QFutureWatcher<std::shared_ptr<PreviewResult>>::finished, this, [this] {
        if (!m_watcher->future().isValid() || m_watcher->future().resultCount() == 0)
            return;
        const auto result = m_watcher->result();
        if (result && result->generation == m_generation)
            applyResult(*result);
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
    m_image->clear();
    m_text->clear();
    m_model->clear();
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
    fillProperties(node, nullptr);
    m_watcher->setFuture(QtConcurrent::run(loadPreview, m_session, node, m_generation));
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
        m_previewStack->setCurrentWidget(m_model);
    } else if (!result.image.isNull()) {
        m_image->setImage(result.image, result.imageInfo);
        m_previewStack->setCurrentWidget(m_image);
    } else if (result.hasText) {
        const int lines = static_cast<int>(result.text.count(QLatin1Char('\n')) + 1);
        QString info = result.textInfo + tr("  ·  %n line(s)", nullptr, lines);
        if (result.textTruncated)
            info += tr("  ·  showing the first %1").arg(QLocale().formattedDataSize(kTextLimit));
        m_text->setText(result.text, result.syntax, info);
        m_text->setEditButtonVisible(!result.decompiled);
        m_previewStack->setCurrentWidget(m_text);
    } else {
        hasPreview = false;
        if (!result.bankError.isEmpty())
            showMessage(tr("Could not read the audio: %1").arg(result.bankError));
        else if (!result.meshError.isEmpty())
            showMessage(tr("Could not read the model: %1").arg(result.meshError));
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
