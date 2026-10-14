#include "SettingsDialog.hpp"

#include "Theme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace viewer::app {

namespace {

int readInt(const QSettings& settings, const char* key, int fallback, int low, int high)
{
    bool ok = false;
    const int value = settings.value(key, fallback).toInt(&ok);
    return ok ? std::clamp(value, low, high) : fallback;
}

QSpinBox* spinBox(QWidget* parent, int low, int high, const QString& suffix, const QString& tip)
{
    auto* box = new QSpinBox(parent);
    box->setRange(low, high);
    box->setSuffix(suffix);
    box->setToolTip(tip);
    return box;
}

} // namespace

AppSettings AppSettings::load()
{
    const QSettings settings;
    AppSettings s;
    s.extractFolder = settings.value("paths/extractFolder").toString();
    s.extractThreads = readInt(settings, "extract/threads", s.extractThreads, 1, 16);
    s.exportThreads = readInt(settings, "extract/exportThreads", s.exportThreads, 1, 16);
    s.packedPreviewMB = readInt(settings, "preview/packedLimitMB", s.packedPreviewMB, 16, 4096);
    s.looseReadMB = readInt(settings, "preview/looseReadMB", s.looseReadMB, 1, 4096);
    s.textMB = readInt(settings, "preview/textMB", s.textMB, 1, 256);
    return s;
}

void AppSettings::save() const
{
    QSettings settings;
    settings.setValue("paths/extractFolder", extractFolder);
    settings.setValue("extract/threads", extractThreads);
    settings.setValue("extract/exportThreads", exportThreads);
    settings.setValue("preview/packedLimitMB", packedPreviewMB);
    settings.setValue("preview/looseReadMB", looseReadMB);
    settings.setValue("preview/textMB", textMB);
}

QString AppSettings::startFolder()
{
    const QString fixed = load().extractFolder;
    if (!fixed.isEmpty() && QDir(fixed).exists())
        return fixed;
    return QSettings().value("paths/lastExtractDir").toString();
}

SettingsDialog::SettingsDialog(ThemeManager& theme, QWidget* parent)
    : QDialog(parent), m_theme(theme), m_themeBox(new QComboBox(this)),
      m_reopen(new QCheckBox(tr("Reopen the last game and selection at start-up"), this)),
      m_lastFolder(new QRadioButton(tr("The last folder used"), this)),
      m_fixedFolder(new QRadioButton(tr("This folder:"), this)), m_folder(new QLineEdit(this)),
      m_extractThreads(spinBox(this, 1, 16, tr(" files"),
                               tr("Plain copies. More helps on an SSD; on a hard disk 2-4 is plenty."))),
      m_exportThreads(spinBox(this, 1, 16, tr(" files"),
                              tr("Converting (models, textures, sounds) uses more memory. Files of 4 MB or more "
                                 "(levels) are always exported one at a time."))),
      m_packedPreview(spinBox(this, 16, 4096, tr(" MB"),
                              tr("Files inside archives are read whole to be previewed. Bigger ones show a "
                                 "message instead (Extract still works)."))),
      m_looseRead(spinBox(this, 1, 4096, tr(" MB"),
                          tr("Loose files bigger than this are previewed and shown in Hex from their start."))),
      m_text(spinBox(this, 1, 256, tr(" MB"), tr("Longer text files show their beginning.")))
{
    setWindowTitle(tr("Settings"));

    // Appearance and start-up
    m_themeBox->addItems({tr("Same as Windows"), tr("Light"), tr("Dark")});
    auto* general = new QGroupBox(tr("Appearance and start-up"), this);
    auto* generalForm = new QFormLayout(general);
    generalForm->addRow(tr("Theme:"), m_themeBox);
    generalForm->addRow(m_reopen);

    // Extract and export
    auto* browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    browse->setToolTip(tr("Choose the folder"));
    m_folder->setPlaceholderText(tr("Folder for extracted and exported files"));
    m_folder->setMinimumWidth(m_folder->fontMetrics().averageCharWidth() * 40);
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(m_fixedFolder);
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    auto* extract = new QGroupBox(tr("Extract and export"), this);
    auto* extractForm = new QFormLayout(extract);
    auto* startLabel = new QLabel(tr("Save dialogs start in:"), this);
    extractForm->addRow(startLabel, m_lastFolder);
    extractForm->addRow(QString(), folderRow);
    extractForm->addRow(tr("Extract at once:"), m_extractThreads);
    extractForm->addRow(tr("Export at once:"), m_exportThreads);

    // Previews
    auto* previews = new QGroupBox(tr("Previews"), this);
    auto* previewForm = new QFormLayout(previews);
    previewForm->addRow(tr("Preview archive files up to:"), m_packedPreview);
    previewForm->addRow(tr("Read loose files up to:"), m_looseRead);
    previewForm->addRow(tr("Show text up to:"), m_text);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::RestoreDefaults,
                                         this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(general);
    layout->addWidget(extract);
    layout->addWidget(previews);
    layout->addStretch(1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            &SettingsDialog::restoreDefaults);
    connect(m_fixedFolder, &QRadioButton::toggled, this, [this, browse](bool on) {
        m_folder->setEnabled(on);
        browse->setEnabled(on);
    });
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder for extracted files"),
                                                              m_folder->text());
        if (!dir.isEmpty())
            m_folder->setText(QDir::toNativeSeparators(dir));
    });

    m_themeBox->setCurrentIndex(static_cast<int>(m_theme.mode()));
    m_reopen->setChecked(QSettings().value("session/reopenOnStartup", true).toBool());
    fill(AppSettings::load());
    m_folder->setEnabled(m_fixedFolder->isChecked());
    browse->setEnabled(m_fixedFolder->isChecked());
}

void SettingsDialog::fill(const AppSettings& s)
{
    (s.extractFolder.isEmpty() ? m_lastFolder : m_fixedFolder)->setChecked(true);
    m_folder->setText(QDir::toNativeSeparators(s.extractFolder));
    m_extractThreads->setValue(s.extractThreads);
    m_exportThreads->setValue(s.exportThreads);
    m_packedPreview->setValue(s.packedPreviewMB);
    m_looseRead->setValue(s.looseReadMB);
    m_text->setValue(s.textMB);
}

void SettingsDialog::restoreDefaults()
{
    fill(AppSettings{});
}

void SettingsDialog::accept()
{
    AppSettings s;
    const QString folder = m_folder->text().trimmed();
    s.extractFolder = m_fixedFolder->isChecked() ? QDir::fromNativeSeparators(folder) : QString();
    s.extractThreads = m_extractThreads->value();
    s.exportThreads = m_exportThreads->value();
    s.packedPreviewMB = m_packedPreview->value();
    s.looseReadMB = m_looseRead->value();
    s.textMB = m_text->value();
    s.save();
    QSettings().setValue("session/reopenOnStartup", m_reopen->isChecked());
    m_theme.setMode(static_cast<ThemeManager::Mode>(m_themeBox->currentIndex()));
    QDialog::accept();
}

} // namespace viewer::app
