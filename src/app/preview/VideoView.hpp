#pragma once

#include <QByteArray>
#include <QWidget>

class QAudioOutput;
class QBuffer;
class QComboBox;
class QLabel;
class QMediaPlayer;
class QSlider;
class QToolButton;
class QVideoWidget;

namespace viewer::app {

/// Video preview (Bink .bik and anything else FFmpeg can play) with play /
/// pause, seeking, volume and audio-track selection (Blur's movies carry
/// separate music / voice tracks).
class VideoView : public QWidget {
    Q_OBJECT
public:
    explicit VideoView(QWidget* parent = nullptr);
    ~VideoView() override;

    /// Plays a file on disk.
    void setFile(const QString& path);
    /// Plays bytes in memory (videos packed inside archives).
    void setData(QByteArray bytes, const QString& name);
    void clear();

private:
    void updateTracks();
    void updateTime();

    QMediaPlayer* m_player;
    QAudioOutput* m_audio;
    QVideoWidget* m_video;
    QBuffer* m_buffer = nullptr;
    QByteArray m_data;

    QLabel* m_info;
    QToolButton* m_play;
    QToolButton* m_stop;
    QSlider* m_position;
    QLabel* m_time;
    QComboBox* m_tracks;
    QSlider* m_volume;
};

} // namespace viewer::app
