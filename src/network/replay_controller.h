#pragma once

#include <QObject>
#include <QFile>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QFuture>
#include <QMap>

#include <memory>
#include <atomic>

#include "core/threadsafe_queue.h"
#include "core/frame_timestamp_reconciler.h"
#include "io/session_manifest.h"

namespace ccv2 {

// Seekable 1x BIN playback transport, modeled on Intan-RHX playback:
// play/pause, jump-to-start/end, rewind/fast-forward, seek-to-position, and a
// live position readout. A timer feeds bounded blocks at 1x; uncached seeks
// build sparse timestamp checkpoints in a cancellable background task.
class ReplayController : public QObject {
    Q_OBJECT
public:
    explicit ReplayController(std::shared_ptr<ThreadSafeQueue<QByteArray>> queue,
                              QObject *parent = nullptr);
    ~ReplayController() override;

    bool open(const QString &binFile, double sampleRate);
    void close();

    bool isOpen() const { return m_file.isOpen(); }
    bool isPlaying() const { return m_timer.isActive(); }
    qint64 totalFrames() const { return m_totalFrames; }
    qint64 currentFrame() const { return m_curFrame; }
    double sampleRate() const { return m_fs; }
    // TDM state recorded in the session manifest; tdmKnown() is false for
    // single-BIN replays and pre-TDM manifests.
    bool tdmKnown() const { return m_tdmKnown; }
    bool tdmEnabled() const { return m_tdmEnabled; }
    bool tdmEvenFirst() const { return m_tdmEvenFirst; }
    QString sourcePath() const { return m_sourcePath; }
    const SessionMetadata &metadata() const { return m_metadata; }
    QJsonObject sourceProvenance() const { return m_sourceProvenance; }
    bool frameValidityKnown() const { return m_frameValidityKnown; }
    bool integrityComplete() const { return m_integrityComplete; }
    bool integrityUnknown() const { return m_integrityUnknown; }
    const QVector<SessionFrameRange> &invalidFrameRanges() const { return m_invalidFrameRanges; }
    QString warning() const { return m_warning; }
    QString errorString() const { return m_error; }
    bool isSeeking() const { return m_seeking; }
    qint64 nextSourceFrame() const { return m_readerTimeline.nextIndex(); }
    void setTimelineEpoch(quint64 epoch) { m_epoch = epoch; }
    // Synchronous, lossless pull for a dedicated offline worker. Never use on
    // the timed GUI transport while playing/seeking or with pending delivery.
    // Empty output with true means EOF; false carries errorString().
    bool readNextBlock(qint64 maximumFrames, QByteArray *bytes, StreamBlockInfo *info);

public slots:
    void play();
    void pause();
    void togglePlay();
    void jumpToStart();
    void jumpToEnd();
    void seekFrame(qint64 frame);
    void seekFraction(double frac);     // 0..1
    void skipSeconds(double seconds);   // signed

signals:
    void positionChanged(qint64 curFrame, qint64 totalFrames);
    void playingChanged(bool playing);
    void backpressureChanged(bool active);
    void finished();
    void seekStarted(qint64 targetFrame);
    void timelinePositioned(qint64 sourceFrame);
    void errorOccurred(const QString &message);

private:
    using ReplayPart = SessionInputPart;

    void tick();
    bool loadParts(const QString &binFile, double fallbackSampleRate);
    bool openPart(int index);
    QByteArray readFrames(qint64 frameCount);
    bool deliverPending();
    void failPlayback(const QString &message);
    void applySeek(qint64 frame, const FrameTimestampReconciler &timeline);
    void cancelSeek();

    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_queue;
    QFile m_file;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QVector<ReplayPart> m_parts;
    int m_partIndex{-1};
    QString m_sourcePath;
    SessionMetadata m_metadata;
    QJsonObject m_sourceProvenance;
    bool m_frameValidityKnown{false};
    bool m_integrityComplete{false};
    bool m_integrityUnknown{false};
    QVector<SessionFrameRange> m_invalidFrameRanges;
    QString m_warning;
    QString m_error;
    double m_fs = 20000.0;
    bool m_tdmKnown{false};
    bool m_tdmEnabled{false};
    bool m_tdmEvenFirst{true};
    int m_frameBytes = 0;
    qint64 m_totalFrames = 0;
    qint64 m_curFrame = 0;
    double m_frameAccum = 0.0;
    QByteArray m_pendingChunk;
    qint64 m_pendingFrames{0};
    bool m_backpressured{false};
    quint64 m_epoch{0};
    StreamBlockInfo m_pendingInfo;
    FrameTimestampReconciler m_readerTimeline;
    QMap<qint64, FrameTimestampReconciler> m_checkpoints;
    bool m_seeking{false};
    bool m_playAfterSeek{false};
    quint64 m_seekGeneration{0};
    std::shared_ptr<std::atomic_bool> m_seekCancel;
    QFuture<void> m_seekFuture;
};

}  // namespace ccv2
