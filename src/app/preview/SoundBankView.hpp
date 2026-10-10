#pragma once

#include "viewer/core/Audio.hpp"

#include <QByteArray>
#include <QBuffer>
#include <QFutureWatcher>
#include <QWidget>

#include <memory>

class QAudioSink;
class QCheckBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace viewer::app {

/// Plays one decoded clip through the default audio output (Qt Multimedia).
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    void setClip(const AudioClip& clip);
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setLooping(bool on) { m_loop = on; }

    [[nodiscard]] bool isPlaying() const noexcept { return m_playing; }
    [[nodiscard]] double position() const;
    [[nodiscard]] double duration() const noexcept { return m_duration; }

signals:
    void stateChanged(bool playing);
    /// Device problems (no output device, unsupported format...).
    void error(const QString& message);

private:
    void startAt(qint64 byteOffset);
    /// Stops and deletes the output and the buffer it was reading.
    void releaseSink();
    void onSinkState();

    QAudioSink* m_sink = nullptr;
    QByteArray m_pcm;
    QBuffer* m_device = nullptr; ///< Read by m_sink only; a new one for every start.
    qint64 m_startByte = 0;
    int m_bytesPerSecond = 0;
    double m_duration = 0;
    bool m_playing = false;
    bool m_loop = false;
    std::uint32_t m_rate = 0;
    std::uint16_t m_channels = 0;
};

/// Lists the sounds in a bank (or one .wav); double-click plays, Export
/// writes .wav files. Sounds are decoded on a worker thread on demand.
class SoundBankView : public QWidget {
    Q_OBJECT
public:
    explicit SoundBankView(QWidget* parent = nullptr);
    ~SoundBankView() override;

    /// `keepAlive` keeps the bank's data (session / archive) valid while shown.
    void setBank(std::shared_ptr<const SoundBank> bank, std::shared_ptr<const void> keepAlive);
    void clear();
    /// Shows "Replace Sound…" (only banks the app can rebuild).
    void setReplaceable(bool on);

signals:
    /// The user asked to replace sound `index` (in bank order) with a .wav file.
    void replaceSoundRequested(int index);

private:
    void showContextMenu(const QPoint& pos);
    void playSelected();
    void exportSounds();
    void updateTime();
    [[nodiscard]] int currentSound() const;

    std::shared_ptr<const SoundBank> m_bank;
    std::shared_ptr<const void> m_keepAlive;
    AudioPlayer* m_player;
    QFutureWatcher<Result<AudioClip>>* m_decode;
    QFutureWatcher<QString>* m_export;
    int m_playingIndex = -1;
    int m_pendingIndex = -1;
    QString m_pendingName;

    QLabel* m_info;
    QTreeWidget* m_list;
    QToolButton* m_play;
    QToolButton* m_stop;
    QSlider* m_position;
    QLabel* m_time;
    QCheckBox* m_loop;
    QToolButton* m_exportButton;
    QToolButton* m_replaceButton;
    QTimer* m_timer;
};

} // namespace viewer::app
