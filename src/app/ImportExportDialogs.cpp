#include "ImportExportDialogs.hpp"

#include "QtConvert.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>

namespace viewer::app {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("ImportExport", text, disambiguation, n);
}

/// A secondary line under an option: smaller, dimmed, wrapped.
QLabel* hint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setContentsMargins(24, 0, 0, 6); // under the option's text, past the radio circle
    return label;
}

} // namespace

std::optional<ImageExport> chooseImageExport(QWidget* parent)
{
    struct Option {
        ImageExport format;
        const char* key;
        QString name;
        QString description;
    };
    const Option options[] = {
        {ImageExport::Png, "png", tr("PNG"),
         tr("Lossless and keeps transparency. Recommended for viewing and editing in any paint program.")},
        {ImageExport::Dds, "dds", tr("DDS"),
         tr("The game's own compressed pixels and mip-maps, unchanged. Best if you want to import the "
            "texture again later. Opens in Paint.NET, GIMP and Photoshop (with a plug-in).")},
        {ImageExport::Jpg, "jpg", tr("JPG"), tr("Small files, but lossy and without transparency.")},
        {ImageExport::Bmp, "bmp", tr("BMP"), tr("Uncompressed; most programs ignore its transparency.")},
    };

    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Export Textures As"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(2);
    layout->addWidget(new QLabel(tr("Save textures as:"), &dialog));
    auto* group = new QButtonGroup(&dialog);
    QSettings settings;
    const QString remembered = settings.value("export/imageFormat", "png").toString();
    for (const Option& o : options) {
        auto* button = new QRadioButton(o.name, &dialog);
        QFont bold = button->font();
        bold.setBold(true);
        button->setFont(bold);
        button->setChecked(remembered == QLatin1String(o.key));
        group->addButton(button, static_cast<int>(o.format));
        layout->addWidget(button);
        layout->addWidget(hint(o.description, &dialog));
    }
    if (!group->checkedButton())
        group->buttons().front()->setChecked(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(460);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    const auto chosen = static_cast<ImageExport>(group->checkedId());
    for (const Option& o : options)
        if (o.format == chosen)
            settings.setValue("export/imageFormat", QLatin1String(o.key));
    return chosen;
}

QString pixelFormatDescription(image::PixelFormat format)
{
    using image::PixelFormat;
    switch (format) {
    case PixelFormat::BC1: return tr("Smallest (½ byte per pixel). No transparency, or only fully on/off.");
    case PixelFormat::BC2: return tr("Sharp transparency in 16 steps. 1 byte per pixel.");
    case PixelFormat::BC3: return tr("Smooth transparency — glass, decals, soft edges. 1 byte per pixel.");
    case PixelFormat::A8R8G8B8: return tr("Uncompressed, exact colours. 4 bytes per pixel (4–8× bigger than DXT).");
    case PixelFormat::A8: return tr("A transparency mask only (shown as greyscale).");
    default: return tr("Uncompressed.");
    }
}

std::optional<image::PixelFormat> chooseTextureFormat(QWidget* parent, const TextureTarget& target,
                                                      const Picture& picture, const QString& pictureName,
                                                      const QString& textureName)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Import Texture"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* intro = new QLabel(&dialog);
    intro->setWordWrap(true);
    const QString mips = target.mipCount > 1 ? tr("%1 mip-map levels").arg(target.mipCount)
                                             : tr("no mip-maps");
    intro->setText(tr("Import <b>%1</b> (%2×%3%4) into <b>%5</b>.<br>The game's texture is %6×%7, %8, %9.")
                       .arg(pictureName.toHtmlEscaped())
                       .arg(picture.image.width)
                       .arg(picture.image.height)
                       .arg(picture.translucent ? tr(", with transparency") : QString())
                       .arg(textureName.toHtmlEscaped())
                       .arg(target.width)
                       .arg(target.height)
                       .arg(toQString(image::pixelFormatName(target.format)))
                       .arg(mips));
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    auto* combo = new QComboBox(&dialog);
    for (const image::PixelFormat f : target.formats) {
        QString text = toQString(image::pixelFormatName(f));
        if (f == target.format)
            text += tr("  (the game's original)");
        combo->addItem(text, static_cast<int>(f));
        if (f == target.format)
            combo->setCurrentIndex(combo->count() - 1);
    }
    form->addRow(tr("Store as:"), combo);
    layout->addLayout(form);

    auto* description = new QLabel(&dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* warning = new QLabel(&dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto update = [&] {
        const auto f = static_cast<image::PixelFormat>(combo->currentData().toInt());
        description->setText(pixelFormatDescription(f));
        QString text;
        if (f == image::PixelFormat::BC1 && picture.translucent)
            text = tr("⚠ The picture has soft transparency; DXT1 keeps it only as on/off. "
                      "DXT5 keeps it smooth.");
        else if (f != target.format)
            text = tr("The game used %1 for this texture; another format usually works, but check it in game.")
                       .arg(toQString(image::pixelFormatName(target.format)));
        warning->setText(text);
        warning->setVisible(!text.isEmpty());
    };
    QObject::connect(combo, &QComboBox::currentIndexChanged, &dialog, update);
    update();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(480);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return static_cast<image::PixelFormat>(combo->currentData().toInt());
}

} // namespace viewer::app
