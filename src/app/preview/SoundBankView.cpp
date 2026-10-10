#include "SoundBankView.hpp"

#include "../Icons.hpp"
#include "../QtConvert.hpp"

#include "viewer/core/RandomAccessFile.hpp"
#include "viewer/core/Vfs.hpp"

#include <QAudioFormat>
#include <QAudioSink>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMediaDevices>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <cstring>

namespace viewer::app {

namespace {

QString timeText(double seconds)
{
    const int s = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// ---- AudioPlayer -------------------------------------------------------------------------

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {}

AudioPlayer::~AudioPlayer()
{
    stop();
}

void AudioPlayer::setClip(const AudioClip& clip)
{
    stop();
    m_pcm = QByteArray(reinterpret_cast<const char*>(clip.samples.data()),
                       static_cast<qsizetype>(clip.samples.size() * sizeof(std::int16_t)));
    m_rate = clip.sampleRate;
    m_channels = clip.channels;
    m_bytesPerSecond = static_cast<int>(clip.sampleRate * clip.channels * 2);
    m_duration = clip.seconds();
    m_startByte = 0;
}

void AudioPlayer::startAt(qint64 byteOffset)
{
    if (m_sink) {
        m_sink->disconnect(this);
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    QAudioFormat format;
    format.setSampleRate(static_cast<int>(m_rate));
    format.setChannelCount(m_channels);
    format.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        emit error(tr("No audio output device found."));
        return;
    }
    m_sink = new QAudioSink(device, format, this);
    connect(m_sink, &QAudioSink::stateChanged, this, &AudioPlayer::onSinkState);
    m_buffer.close();
    m_buffer.setBuffer(&m_pcm);
    m_buffer.open(QIODevice::ReadOnly);
    const int frameBytes = m_channels * 2;
    m_startByte = std::clamp<qint64>(byteOffset - byteOffset % std::max(1, frameBytes), 0, m_pcm.size());
    m_buffer.seek(m_startByte);
    m_sink->start(&m_buffer);
    m_playing = true;
    emit stateChanged(true);
}

void AudioPlayer::onSinkState()
{
    if (!m_sink)
        return;
    if (m_sink->state() == QAudio::IdleState && m_buffer.atEnd()) { // reached the end
        if (m_loop) {
            startAt(0);
            return;
        }
        stop();
        m_startByte = 0;
    } else if (m_sink->state() == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
        emit error(tr("Audio playback failed (error %1).").arg(static_cast<int>(m_sink->error())));
        stop();
    }
}

void AudioPlayer::play()
{
    if (m_pcm.isEmpty())
        return;
    if (m_sink && m_sink->state() == QAudio::SuspendedState) {
        m_sink->resume();
        m_playing = true;
        emit stateChanged(true);
        return;
    }
    startAt(m_startByte >= m_pcm.size() ? 0 : m_startByte);
}

void AudioPlayer::pause()
{
    if (m_sink && m_playing) {
        m_sink->suspend();
        m_playing = false;
        emit stateChanged(false);
    }
}

void AudioPlayer::stop()
{
    if (m_sink) {
        m_sink->disconnect(this);
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_buffer.close();
    const bool was = m_playing;
    m_playing = false;
    m_startByte = 0;
    if (was)
        emit stateChanged(false);
}

void AudioPlayer::seek(double seconds)
{
    const auto byte = static_cast<qint64>(seconds * m_bytesPerSecond);
    if (m_sink && m_playing)
        startAt(byte);
    else
        m_startByte = byte - byte % std::max(1, m_channels * 2);
}

double AudioPlayer::position() const
{
    if (!m_bytesPerSecond)
        return 0;
    double seconds = static_cast<double>(m_startByte) / m_bytesPerSecond;
    if (m_sink)
        seconds += static_cast<double>(m_sink->processedUSecs()) / 1e6;
    return std::min(seconds, m_duration);
}

// ---- SoundBankView -----------------------------------------------------------------------

SoundBankView::SoundBankView(QWidget* parent)
    : QWidget(parent), m_player(new AudioPlayer(this)), m_decode(new QFutureWatcher<Result<AudioClip>>(this)),
      m_export(new QFutureWatcher<QString>(this)), m_info(new QLabel(this)), m_list(new QTreeWidget(this)),
      m_play(new QToolButton(this)), m_stop(new QToolButton(this)), m_position(new QSlider(Qt::Horizontal, this)),
      m_time(new QLabel(this)), m_loop(new QCheckBox(tr("Loop"), this)), m_exportButton(new QToolButton(this)),
      m_timer(new QTimer(this))
{
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_list->setHeaderLabels({tr("Sound"), tr("Length"), tr("Channels"), tr("Rate"), tr("Format"), tr("Loop")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAllColumnsShowFocus(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(-1, Qt::AscendingOrder); // bank order until the user sorts
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setStretchLastSection(false);
    connect(m_list, &QTreeWidget::itemActivated, this, &SoundBankView::playSelected);

    using TI = QIcon::ThemeIcon;
    m_play->setIcon(icons::theme(TI::MediaPlaybackStart, QStyle::SP_MediaPlay));
    m_play->setToolTip(tr("Play / pause (double-click a sound to play it)"));
    m_stop->setIcon(icons::theme(TI::MediaPlaybackStop, QStyle::SP_MediaStop));
    m_stop->setToolTip(tr("Stop"));
    m_exportButton->setIcon(icons::theme(TI::DocumentSaveAs, QStyle::SP_DialogSaveButton));
    m_exportButton->setText(tr("Export WAV…"));
    m_exportButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_exportButton->setToolTip(tr("Save the selected sounds (or all) as .wav files"));
    for (QToolButton* b : {m_play, m_stop, m_exportButton})
        b->setAutoRaise(true);
    m_loop->setToolTip(tr("Repeat the sound (Blur marks looping sounds in the Loop column)"));
    m_position->setRange(0, 1000);
    m_position->setEnabled(false);
    m_time->setText(timeText(0) + QStringLiteral(" / ") + timeText(0));

    auto* top = new QHBoxLayout;
    top->setContentsMargins(8, 4, 8, 4);
    top->addWidget(m_info, 1);
    top->addWidget(m_exportButton);

    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(8, 4, 8, 6);
    controls->addWidget(m_play);
    controls->addWidget(m_stop);
    controls->addWidget(m_position, 1);
    controls->addWidget(m_time);
    controls->addWidget(m_loop);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(m_list, 1);
    layout->addLayout(controls);

    connect(m_play, &QToolButton::clicked, this, [this] {
        if (m_player->isPlaying())
            m_player->pause();
        else if (m_playingIndex >= 0 && m_playingIndex == currentSound())
            m_player->play();
        else
            playSelected();
    });
    connect(m_stop, &QToolButton::clicked, m_player, &AudioPlayer::stop);
    connect(m_loop, &QCheckBox::toggled, m_player, &AudioPlayer::setLooping);
    connect(m_position, &QSlider::sliderReleased, this,
            [this] { m_player->seek(m_player->duration() * m_position->value() / 1000.0); });
    connect(m_player, &AudioPlayer::stateChanged, this, [this](bool playing) {
        m_play->setIcon(icons::theme(playing ? QIcon::ThemeIcon::MediaPlaybackPause
                                             : QIcon::ThemeIcon::MediaPlaybackStart,
                                     playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        playing ? m_timer->start() : m_timer->stop();
        updateTime();
    });
    connect(m_player, &AudioPlayer::error, this, [this](const QString& message) { m_info->setText(message); });
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, &SoundBankView::updateTime);

    connect(m_decode, &QFutureWatcher<Result<AudioClip>>::finished, this, [this] {
        if (m_decode->future().resultCount() == 0)
            return;
        const auto result = m_decode->result();
        const int index = std::exchange(m_pendingIndex, -1);
        if (!result) {
            m_info->setText(tr("Could not decode: %1").arg(toQString(result.error().message)));
            return;
        }
        m_info->setText(tr("Playing %1").arg(m_pendingName));
        m_playingIndex = index;
        m_player->setClip(*result);
        m_player->setLooping(m_loop->isChecked());
        m_position->setEnabled(true);
        m_player->play();
    });
    connect(m_export, &QFutureWatcher<QString>::finished, this, [this] {
        m_exportButton->setEnabled(true);
        if (m_export->future().resultCount())
            m_info->setText(m_export->result());
    });
    connect(m_exportButton, &QToolButton::clicked, this, &SoundBankView::exportSounds);
}

SoundBankView::~SoundBankView()
{
    m_player->stop();
    m_decode->waitForFinished();
    m_export->waitForFinished();
}

void SoundBankView::setBank(std::shared_ptr<const SoundBank> bank, std::shared_ptr<const void> keepAlive)
{
    clear();
    m_bank = std::move(bank);
    m_keepAlive = std::move(keepAlive);
    if (!m_bank)
        return;
    const QLocale locale;
    double total = 0;
    const auto sounds = m_bank->sounds();
    m_list->setSortingEnabled(false);
    for (std::size_t i = 0; i < sounds.size(); ++i) {
        const SoundInfo& s = sounds[i];
        auto* item = new QTreeWidgetItem(m_list);
        item->setText(0, toQString(s.name));
        item->setText(1, timeText(s.seconds()));
        item->setData(1, Qt::UserRole, s.seconds());
        item->setText(2, s.channels == 1 ? tr("Mono") : s.channels == 2 ? tr("Stereo") : QString::number(s.channels));
        item->setText(3, QStringLiteral("%1 Hz").arg(s.sampleRate));
        item->setText(4, toQString(s.codec));
        item->setText(5, s.looped ? tr("Yes") : QString());
        item->setData(0, Qt::UserRole, static_cast<int>(i));
        item->setIcon(0, icons::forCategory(FileCategory::Audio));
        total += s.seconds();
    }
    m_list->setSortingEnabled(true);
    for (int c = 1; c < m_list->columnCount(); ++c)
        m_list->resizeColumnToContents(c);
    m_info->setText(tr("%1  ·  %n sound(s)  ·  %2 in total", nullptr, static_cast<int>(sounds.size()))
                        .arg(toQString(m_bank->formatName()), timeText(total)));
    if (m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(0));
}

void SoundBankView::clear()
{
    m_player->stop();
    m_player->setClip({});
    m_decode->waitForFinished();
    m_list->clear();
    m_info->clear();
    m_bank.reset();
    m_keepAlive.reset();
    m_playingIndex = m_pendingIndex = -1;
    m_position->setValue(0);
    m_position->setEnabled(false);
    updateTime();
}

int SoundBankView::currentSound() const
{
    const QTreeWidgetItem* item = m_list->currentItem();
    return item ? item->data(0, Qt::UserRole).toInt() : -1;
}

void SoundBankView::playSelected()
{
    const int index = currentSound();
    if (!m_bank || index < 0 || m_decode->isRunning())
        return;
    m_player->stop();
    m_pendingIndex = index;
    m_info->setText(tr("Decoding %1…").arg(m_list->currentItem()->text(0)));
    auto bank = m_bank;
    auto keep = m_keepAlive;
    m_pendingName = m_list->currentItem()->text(0);
    m_decode->setFuture(QtConcurrent::run([bank, keep, index] { return bank->decode(static_cast<std::size_t>(index)); }));
}

void SoundBankView::updateTime()
{
    const double pos = m_player->position();
    const double len = m_player->duration();
    m_time->setText(timeText(pos) + QStringLiteral(" / ") + timeText(len));
    if (!m_position->isSliderDown())
        m_position->setValue(len > 0 ? static_cast<int>(1000.0 * pos / len) : 0);
}

void SoundBankView::exportSounds()
{
    if (!m_bank || m_export->isRunning())
        return;
    std::vector<int> indices;
    for (const QTreeWidgetItem* item : m_list->selectedItems())
        indices.push_back(item->data(0, Qt::UserRole).toInt());
    if (indices.empty())
        for (int i = 0; i < static_cast<int>(m_bank->sounds().size()); ++i)
            indices.push_back(i);

    QSettings settings;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Export %n sound(s) as WAV to", nullptr, static_cast<int>(indices.size())),
        settings.value("paths/lastExtractDir").toString());
    if (dir.isEmpty())
        return;
    settings.setValue("paths/lastExtractDir", dir);
    m_exportButton->setEnabled(false);
    m_info->setText(tr("Exporting %n sound(s)…", nullptr, static_cast<int>(indices.size())));
    auto bank = m_bank;
    auto keep = m_keepAlive;
    const auto target = toPath(dir);
    m_export->setFuture(QtConcurrent::run([bank, keep, indices, target]() -> QString {
        int written = 0;
        QStringList errors;
        for (const int i : indices) {
            const SoundInfo& info = bank->sounds()[static_cast<std::size_t>(i)];
            auto clip = bank->decode(static_cast<std::size_t>(i));
            if (!clip) {
                errors << toQString(clip.error().message);
                continue;
            }
            const auto file = target / sanitizeRelativePath(info.name + ".wav");
            if (auto ok = writeFile(file, audio::encodeWav(*clip)); ok)
                ++written;
            else
                errors << toQString(ok.error().message);
        }
        QString text = QObject::tr("Exported %n sound(s) to %1", nullptr, written)
                           .arg(QDir::toNativeSeparators(toQString(target)));
        if (!errors.isEmpty())
            text += QObject::tr("  ·  %n failed: ", nullptr, static_cast<int>(errors.size())) + errors.front();
        return text;
    }));
}

} // namespace viewer::app
