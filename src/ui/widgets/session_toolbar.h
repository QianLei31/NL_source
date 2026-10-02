#pragma once

#include <QWidget>
#include <QString>

class QPushButton;
class QLabel;
class QSlider;
class QStackedWidget;
class QPropertyAnimation;
class QGraphicsOpacityEffect;

namespace ccv2 {

// Global session control bar (toolbar row under the title): the single point
// of control for the whole app's data timeline. Display pages are pure views.
//
// One mode's controls visible at a time (QStackedWidget), switched by the
// [实时 | 回放] segment. The segment itself is a pure view switch — the real
// mode change happens through the actions:
//   实时 page:  [▶ 开始接收/■ 停止接收] 采集状态  [REC计时] [● 录制]
//   回放 page:  [打开文件…] [file.bin ×] ⏮◀◀▶▶▶⏭ ═slider═ 04:12/12:00
class SessionToolbar : public QWidget {
    Q_OBJECT
public:
    explicit SessionToolbar(QWidget *parent = nullptr);

    // State feedback (driven by the main window / hub).
    void setRunning(bool running);                 // live connected/streaming
    void setRecording(bool recording, const QString &text);
    void setReplayLoaded(const QString &file);
    void setReplayPlaying(bool playing);
    void setReplayError(const QString &message);
    void setReplayPosition(qint64 curFrame, qint64 totalFrames, double sampleRate);
    // Acquisition state next to the receive button (e.g. 待机/连接中/接收中).
    void setLiveConnection(const QString &text, const char *state);
    // Ingress rate while receiving; pass a negative value to hide.
    void setNetworkRate(qint64 bytesPerSec);

signals:
    void runToggleRequested();          // live receive start/stop
    void recordToggleRequested();
    void replayLoadRequested(const QString &binFile);
    void replayTogglePlayRequested();
    void replayJumpStartRequested();
    void replayJumpEndRequested();
    void replaySkipRequested(double seconds);
    void replaySeekRequested(double frac);
    void replayStopRequested();         // chip × — unload the file

private:
    void updateControls();

    // mode segment (view switch only)
    QPushButton *m_segLiveBtn = nullptr;
    QPushButton *m_segReplayBtn = nullptr;
    QStackedWidget *m_modeStack = nullptr;

    // live page
    QPushButton *m_runBtn = nullptr;
    QLabel *m_connLabel = nullptr;
    QLabel *m_rateLabel = nullptr;
    QLabel *m_recInfoLabel = nullptr;
    QPushButton *m_recordBtn = nullptr;

    // replay page
    QPushButton *m_loadBinBtn = nullptr;
    QWidget *m_fileChip = nullptr;
    QLabel *m_replayFileLabel = nullptr;
    QPushButton *m_fileCloseBtn = nullptr;
    QPushButton *m_jumpStartBtn = nullptr;
    QPushButton *m_rwBtn = nullptr;
    QPushButton *m_playBtn = nullptr;
    QPushButton *m_ffBtn = nullptr;
    QPushButton *m_jumpEndBtn = nullptr;
    QSlider *m_seekSlider = nullptr;
    QLabel *m_replayPosLabel = nullptr;
    QString m_lastDir;
    bool m_seekDragging = false;

    QGraphicsOpacityEffect *m_recPulseFx = nullptr;
    QPropertyAnimation *m_recPulse = nullptr;

    bool m_running = false;
    bool m_recording = false;
    bool m_replayLoaded = false;
};

}  // namespace ccv2
