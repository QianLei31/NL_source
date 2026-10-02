#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

#include "core/channel_routing.h"
#include "core/frame_timestamp_reconciler.h"
#include "core/threadsafe_queue.h"
#include "network/timestamp_checker.h"
#include "io/session_manifest.h"

namespace ccv2 {

class RecordingWorker;
class ReplayController;
class SessionDistributor;
class SocketReceiver2;
class TdmContext;

class SessionHub : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,
        ConnectingLive,
        Live,
        ReplayReady,
        ReplayPlaying,
        ReplayPaused,
        Stopping,
        Error,
    };
    Q_ENUM(State)

    struct Statistics {
        qint64 receivedBytes{0};
        qint64 distributedFrames{0};
        qint64 ingressDroppedFrames{0};
        qint64 subscriberDroppedFrames{0};
        qint64 recordedBytes{0};
        qint64 upstreamMissingFrames{0};
    };

    explicit SessionHub(QObject *parent = nullptr);
    ~SessionHub() override;

    void addSubscriber(const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue);
    qint64 addSubscriberWithFrameOrigin(
        const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue);
    void removeSubscriber(const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue);
    // Detach a scientific consumer without discarding its accepted tail.
    void detachSubscriberForDrain(const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue);

    bool start(const QString &host,
               int controlPort,
               int dataPort,
               const QString &cmd = QStringLiteral("ctre"),
               double sampleRate = 20000.0);
    bool startReplay(const QString &binFile, double fallbackSampleRate);
    void stop();

    State state() const {
        return static_cast<State>(m_state.load());
    }
    bool isRunning() const;
    bool isConnected() const { return m_connected.load(); }
    bool isReplaying() const;
    double sampleRate() const { return m_sampleRate.load(); }

    void replayTogglePlay();
    void replayPause();
    void replayJumpStart();
    void replayJumpEnd();
    void replaySkip(double seconds);
    void replaySeekFraction(double frac);

    bool startRecording(const QString &baseDir,
                        const QString &sessionName,
                        qint64 splitBytes = 0);
    void stopRecording();

    // Threshold-triggered recording: while enabled and not already recording,
    // a crossing of `thresholdV` on `channel` (in the chosen direction) emits
    // triggerFired(), which the main window turns into a recording start.
    struct TriggerConfig {
        bool enabled = false;
        int channel = 0;
        double thresholdV = 0.9;
        bool risingAbove = true;  // false = falling below
        // TDM electrode select: -1 = evaluate every frame (non-TDM); 0/1 =
        // first/second electrode in the selected 0/2 or 1/3 phase pair.
        int tdmSlot = -1;
    };
    void setTriggerConfig(const TriggerConfig &cfg);
    // Whether an absolute frame carries the trigger's selected TDM electrode.
    // tdmSlot < 0 (non-TDM) accepts every frame. Static + pure so the parity
    // mapping can be verified directly.
    static bool triggerFrameMatchesSlot(qint64 absoluteFrame,
                                        bool pair02,
                                        int tdmSlot) {
        if (tdmSlot < 0) {
            return true;
        }
        if (tdmSlot > 1) {
            return false;
        }
        const QPair<int, int> pair = tdmLocalElePair(pair02);
        const int selectedPhase = tdmSlot == 0 ? pair.first : pair.second;
        return tdmPhaseForFrame(absoluteFrame) == selectedPhase;
    }
    bool isRecording() const { return m_recording.load(); }
    qint64 recordedBytes() const;
    QString recordingPath() const;
    Statistics statistics() const;
    quint64 timelineEpoch() const { return m_timelineEpoch.load(); }
    qint64 currentFrameIndex() const;
    // For consumer threads (sorters): read the epoch without signal latency so
    // stale in-flight chunks can be dropped at commit time. The hub outlives
    // all page threads (closeEvent stops pages first).
    const std::atomic<quint64> *timelineEpochCounter() const { return &m_timelineEpoch; }
    const std::atomic<qint64> *timelineFrameOriginCounter() const {
        return &m_timelineFrameOrigin;
    }

    // Application-wide TDM state (hardware interleave on/off + phase). Seeded
    // from config at startup, snapshotted into session.json on recording and
    // restored from the manifest on replay.
    TdmContext *tdmContext() const { return m_tdmContext; }

    // Global software reference (common-mode removal), applied by the array
    // display sorters. 0=off, 1=common-average (CAR), 2=median.
    enum ReferenceMode { RefOff = 0, RefCommonAverage = 1, RefMedian = 2 };
    void setReferenceMode(int mode);
    void setAnalysisMetadata(const QJsonObject &metadata);
    QJsonObject analysisMetadata() const;
    int referenceMode() const { return m_referenceMode.load(); }
    const std::atomic<int> *referenceModeCounter() const { return &m_referenceMode; }

signals:
    void stateChanged(ccv2::SessionHub::State state);
    void referenceModeChanged(int mode);
    void replayAnalysisMetadataAvailable(const QJsonObject &metadata);
    void connectionEvent(const QString &message);
    void controlLinkResult(bool reachable, const QString &message);
    void connectionStateChanged(bool connected);
    void framesDropped(qint64 frames);
    void statisticsChanged();
    void recordingStateChanged(bool recording, const QString &path);
    void recordingError(const QString &message);
    void triggerFired(quint64 epoch);
    void replayFinished();
    // Producer and distributor have handed off every accepted frame. Direct
    // consumers may drain their queues before epoch invalidation.
    void sourceDrained(quint64 epoch, bool eof, bool cleanStop);
    void replayError(const QString &message);
    void replayPosition(qint64 curFrame, qint64 totalFrames);
    void replayPlayingChanged(bool playing);
    void replayBackpressureChanged(bool active);
    void timelineReset(quint64 epoch, qint64 targetFrame);

private:
    friend class SessionDistributor;

    bool dispatchNext(const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue);
    void onChunkLocked(const QByteArray &chunk, qint64 droppedBytesBefore, const StreamBlockInfo &sourceInfo);
    void checkTrigger(const QByteArray &chunk, const StreamBlockInfo &info);
    SessionIntegrity recordingIntegrityLocked() const;
    void setState(State state);
    void stopInternal(State finalState, const QString &reason);
    void resetTimeline(qint64 targetFrame, bool notifyViews = true);
    void finishReplayAfterDistribution(quint64 epoch);
    void performReplaySeek(qint64 targetFrame);
    void cleanupRecordingWorker();
    // Atomically (w.r.t. the distributor's m_dispatchMutex) clears
    // m_recording + m_recordWorker and returns the detached worker, so the
    // caller can stop/delete it without racing onChunkLocked().
    RecordingWorker *detachRecordingWorker(bool *wasRecording);

    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_hubQueue;
    std::unique_ptr<std::atomic_bool> m_stopFlag;
    std::atomic_bool m_distributorStop{false};
    TdmContext *m_tdmContext{nullptr};
    SocketReceiver2 *m_receiver{nullptr};
    ReplayController *m_replay{nullptr};
    SessionDistributor *m_distributor{nullptr};
    RecordingWorker *m_recordWorker{nullptr};

    mutable QMutex m_subMutex;
    mutable QMutex m_dispatchMutex;
    QVector<std::shared_ptr<ThreadSafeQueue<QByteArray>>> m_subscribers;

    std::atomic<int> m_state{static_cast<int>(State::Idle)};
    std::atomic<int> m_referenceMode{0};
    std::atomic_bool m_connected{false};
    std::atomic_bool m_recording{false};
    std::atomic<double> m_sampleRate{20000.0};
    std::atomic<quint64> m_timelineEpoch{0};
    std::atomic<qint64> m_timelineFrameOrigin{0};
    std::atomic<qint64> m_currentFrameIndex{0};
    std::atomic<qint64> m_receivedBytes{0};
    std::atomic<qint64> m_distributedFrames{0};
    std::atomic<qint64> m_ingressDroppedFrames{0};
    std::atomic<qint64> m_subscriberDroppedFrames{0};
    std::atomic<qint64> m_lastRecordedBytes{0};

    QMutex m_triggerMutex;
    TriggerConfig m_trigger;          // written on GUI thread, read on distributor
    double m_triggerPrevValue{0.0};   // distributor-thread only
    bool m_triggerPrimed{false};      // distributor-thread only
    FrameTimestampReconciler m_sourceTimeline;
    TimestampContinuityAnalyzer m_integrityAnalyzer;
    SessionIntegrity m_integrity;
    bool m_timestampCounterObserved{false};
    SessionIntegrity m_recordingIntegrityBase;
    qint64 m_recordingIngressBase{0};
    std::atomic<qint64> m_upstreamMissingFrames{0};

    QString m_sourceType{QStringLiteral("live")};
    QString m_host;
    int m_controlPort{0};
    int m_dataPort{0};
    QString m_command{QStringLiteral("ctre")};
    QString m_recordingPath;
    QJsonObject m_analysisMetadata;
    QJsonObject m_recordingAnalysisMetadata;
};

}  // namespace ccv2
