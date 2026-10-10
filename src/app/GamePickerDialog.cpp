#include "GamePickerDialog.hpp"

#include "Icons.hpp"
#include "QtConvert.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace viewer::app {

namespace {

QString settingsKey(const Game& game)
{
    return QStringLiteral("games/%1/path").arg(toQString(game.info().id));
}

} // namespace

GamePickerDialog::GamePickerDialog(const GameRegistry& games, QWidget* parent)
    : QDialog(parent), m_games(games), m_list(new QListWidget(this)), m_path(new QLineEdit(this)),
      m_status(new QLabel(this)), m_open(nullptr)
{
    setWindowTitle(tr("Open Game"));
    setMinimumWidth(560);

    auto* heading = new QLabel(tr("Which game do you want to browse?"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.3);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);

    auto* hint = new QLabel(tr("Games available in this build. Add more by creating a module under "
                               "src/games/ and listing it in VIEWER_GAMES."),
                            this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);

    m_list->setIconSize(QSize(32, 32));
    m_list->setSpacing(2);
    m_list->setMinimumHeight(140);
    for (const auto& game : games.games()) {
        const auto& info = game->info();
        auto* item = new QListWidgetItem(icons::appIcon(),
                                         QStringLiteral("%1\n%2 · %3").arg(toQString(info.title),
                                                                           toQString(info.developer))
                                             .arg(info.year),
                                         m_list);
        item->setToolTip(toQString(info.description));
        item->setData(Qt::UserRole, toQString(info.id));
    }

    auto* browseButton = new QPushButton(tr("Browse…"), this);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseButton);
    m_path->setPlaceholderText(tr("Game install folder"));
    m_path->setClearButtonEnabled(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Install folder:"), pathRow);
    form->addRow(QString(), m_status);
    m_status->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_open = buttons->addButton(tr("Open"), QDialogButtonBox::AcceptRole);
    m_open->setDefault(true);
    auto* archiveButton = buttons->addButton(tr("Open Archive File…"), QDialogButtonBox::ActionRole);
    archiveButton->setToolTip(tr("Browse a single archive without choosing a game"));

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addWidget(m_list, 1);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &GamePickerDialog::onGameChanged);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (m_open->isEnabled())
            accept();
    });
    connect(m_path, &QLineEdit::textChanged, this, &GamePickerDialog::validate);
    connect(browseButton, &QPushButton::clicked, this, &GamePickerDialog::browse);
    connect(buttons, &QDialogButtonBox::accepted, this, &GamePickerDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &GamePickerDialog::reject);
    connect(archiveButton, &QPushButton::clicked, this, [this] {
        m_wantsArchive = true;
        QDialog::accept();
    });

    // Preselect the last game used.
    const QString last = QSettings().value("games/last").toString();
    int row = 0;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toString() == last)
            row = i;
    m_list->setCurrentRow(row);
    if (m_list->count() == 0)
        validate();
}

const Game* GamePickerDialog::selectedGame() const
{
    const auto* item = m_list->currentItem();
    return item ? m_games.find(item->data(Qt::UserRole).toString().toStdString()) : nullptr;
}

std::filesystem::path GamePickerDialog::installPath() const
{
    return toPath(QDir::cleanPath(m_path->text().trimmed()));
}

void GamePickerDialog::onGameChanged()
{
    const Game* game = selectedGame();
    if (!game) {
        validate();
        return;
    }
    QString path = QSettings().value(settingsKey(*game)).toString();
    if (path.isEmpty()) {
        for (const auto& candidate : game->suggestInstallPaths()) {
            if (game->detectInstall(candidate)) {
                path = QDir::toNativeSeparators(toQString(candidate));
                break;
            }
        }
    }
    m_path->setText(path);
    validate();
}

void GamePickerDialog::validate()
{
    const Game* game = selectedGame();
    const QString text = m_path->text().trimmed();
    const bool exists = !text.isEmpty() && QDir(text).exists();
    m_open->setEnabled(game && exists);

    if (!game) {
        m_status->setText(tr("No game modules were compiled into this build."));
        return;
    }
    const QString title = toQString(game->info().title);
    if (text.isEmpty())
        m_status->setText(tr("Choose the folder where %1 is installed.").arg(title));
    else if (!exists)
        m_status->setText(tr("⚠ That folder does not exist."));
    else if (game->detectInstall(installPath()))
        m_status->setText(tr("✔ %1 install detected.").arg(title));
    else
        m_status->setText(tr("⚠ This doesn't look like a %1 install — loose files will still be shown.").arg(title));
}

void GamePickerDialog::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Install Folder"), m_path->text());
    if (!dir.isEmpty())
        m_path->setText(QDir::toNativeSeparators(dir));
}

void GamePickerDialog::accept()
{
    if (const Game* game = selectedGame()) {
        QSettings settings;
        settings.setValue("games/last", toQString(game->info().id));
        settings.setValue(settingsKey(*game), m_path->text().trimmed());
    }
    QDialog::accept();
}

} // namespace viewer::app
