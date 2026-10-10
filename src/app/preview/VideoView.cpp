#include "VideoView.hpp"

#include "../Icons.hpp"

#include <QAudioOutput>
#include <QBuffer>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace viewer::app {

namespace {

QString timeText(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

VideoView::VideoView(QWidget* parent)
    : QWidget(parent), m_player(new QMediaPlayer(this)), m_audio(new QAudioOutput(this)),
      m_video(new QVideoWidget(this)), m_info(new QLabel(this)), m_play(new QToolButton(this)),
      m_stop(new QToolButton(this)), m_position(new QSlider(Qt::Horizontal, this)), m_time(new QLabel(this)),
      m_tracks(new QComboBox(this)), m_volume(new QSlider(Qt::Horizontal, this))
{
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video);
    m_video->setAspectRatioMode(Qt::KeepAspectRatio);
    QPalette black = m_video->palette();
    black.setColor(QPalette::Window, Qt::black);
    m_video->setPalette(black);
    m_video->setAutoFillBackground(true);

    using TI = QIcon::ThemeIcon;
    m_play->setIcon(icons::theme(TI::MediaPlaybackStart, QStyle::SP_MediaPlay));
    m_play->setToolTip(tr("Play / pause"));
    m_stop->setIcon(icons::theme(TI::MediaPlaybackStop, QStyle::SP_MediaStop));
    m_stop->setToolTip(tr("Stop"));
    m_play->setAutoRaise(true);
    m_stop->setAutoRaise(true);
    m_tracks->setToolTip(tr("Audio track"));
    m_tracks->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_volume->setRange(0, 100);
    m_volume->setValue(QSettings().value("preview/volume", 80).toInt());
    m_volume->setFixedWidth(90);
    m_volume->setToolTip(tr("Volume"));
    m_audio->setVolume(static_cast<float>(m_volume->value()) / 100.0f);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(8, 4, 8, 4);
    top->addWidget(m_info, 1);
    top->addWidget(m_tracks);

    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(8, 4, 8, 6);
    controls->addWidget(m_play);
    controls->addWidget(m_stop);
    controls->addWidget(m_position, 1);
    controls->addWidget(m_time);
    controls->addSpacing(8);
    controls->addWidget(new QLabel(tr("Volume"), this));
    controls->addWidget(m_volume);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(m_video, 1);
    layout->addLayout(controls);

    connect(m_play, &QToolButton::clicked, this, [this] {
        m_player->playbackState() == QMediaPlayer::PlayingState ? m_player->pause() : m_player->play();
    });
    connect(m_stop, &QToolButton::clicked, m_player, &QMediaPlayer::stop);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        const bool playing = state == QMediaPlayer::PlayingState;
        m_play->setIcon(icons::theme(playing ? QIcon::ThemeIcon::MediaPlaybackPause
                                             : QIcon::ThemeIcon::MediaPlaybackStart,
                                     playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
        m_position->setRange(0, static_cast<int>(ms));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (!m_position->isSliderDown())
            m_position->setValue(static_cast<int>(ms));
        updateTime();
    });
    connect(m_position, &QSlider::sliderMoved, m_player, [this](int ms) { m_player->setPosition(ms); });
    connect(m_player, &QMediaPlayer::tracksChanged, this, &VideoView::updateTracks);
    connect(m_player, &QMediaPlayer::metaDataChanged, this, &VideoView::updateTracks);
    connect(m_tracks, &QComboBox::activated, this, [this](int index) { m_player->setActiveAudioTrack(index); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        m_audio->setVolume(static_cast<float>(v) / 100.0f);
        QSettings().setValue("preview/volume", v);
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& message) { m_info->setText(tr("Can't play: %1").arg(message)); });
}

VideoView::~VideoView()
{
    clear();
}

void VideoView::setFile(const QString& path)
{
    clear();
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
}

void VideoView::setData(QByteArray bytes, const QString& name)
{
    clear();
    m_data = std::move(bytes);
    m_buffer = new QBuffer(&m_data, this);
    m_buffer->open(QIODevice::ReadOnly);
    m_player->setSourceDevice(m_buffer, QUrl(name));
    m_player->play();
}

void VideoView::clear()
{
    m_player->stop();
    m_player->setSource({});
    if (m_buffer) {
        m_buffer->deleteLater();
        m_buffer = nullptr;
    }
    m_data.clear();
    m_info->clear();
    m_tracks->clear();
    m_tracks->hide();
}

void VideoView::updateTracks()
{
    const auto tracks = m_player->audioTracks();
    m_tracks->clear();
    for (int i = 0; i < tracks.size(); ++i) {
        QString label = tr("Audio track %1").arg(i + 1);
        if (const QString title = tracks[i].stringValue(QMediaMetaData::Title); !title.isEmpty())
            label += QStringLiteral(" — ") + title;
        m_tracks->addItem(label);
    }
    m_tracks->setCurrentIndex(m_player->activeAudioTrack());
    m_tracks->setVisible(tracks.size() > 1);

    const QSize size = m_player->metaData().value(QMediaMetaData::Resolution).toSize();
    QStringList parts;
    if (size.isValid())
        parts << QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
    if (const QString codec = m_player->metaData().stringValue(QMediaMetaData::VideoCodec);
        !codec.isEmpty() && codec != QLatin1String("Unspecified"))
        parts << codec;
    if (tracks.size() > 0)
        parts << tr("%n audio track(s)", nullptr, static_cast<int>(tracks.size()));
    m_info->setText(parts.join(QStringLiteral("  ·  ")));
}

void VideoView::updateTime()
{
    m_time->setText(timeText(m_player->position()) + QStringLiteral(" / ") + timeText(m_player->duration()));
}

} // namespace viewer::app
