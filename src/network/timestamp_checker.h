#pragma once

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

#include "core/threadsafe_queue.h"

namespace ccv2 {

struct TimestampContinuityStats {
    qint64 framesSeen{0};
    qint64 transitionsChecked{0};
    qint64 discontinuities{0};
    qint64 estimatedMissingFrames{0};
    qint64 repeatedFrames{0};
    qint64 irregularJumps{0};
    qint64 intraFrameMismatchFrames{0};
    qint64 mismatchedChannelWords{0};
    qint64 firstErrorFrame{-1};
    quint32 firstErrorExpected{0};
    quint32 firstErrorActual{0};
    quint32 firstTimestamp{0};
    quint32 lastTimestamp{0};
    quint32 expectedStep{0};
    bool hasData{false};
    bool calibrated{false};
};

class TimestampContinuityAnalyzer {
public:
    explicit TimestampContinuityAnalyzer(quint32 expectedStep = 1);

    void reset(quint32 expectedStep = 1);
    void process(const QByteArray &bytes);
    TimestampContinuityStats stats() const;

    // Intra-frame consistency (all 256 channel words share one timestamp) is the
    // expensive part. stride N checks it on every Nth frame (1=every frame,
    // <=0=never). Continuity of channel 0 is always checked (nearly free).
    void setIntraFrameStride(int stride) { m_intraFrameStride = stride; }

private:
    void processFrame(const char *frame);
    void finishAutoCalibration();
    void checkTransition(quint32 previousTimestamp,
                         quint32 actualTimestamp,
                         qint64 frameIndex);

    QByteArray m_leftover;
    TimestampContinuityStats m_stats;
    quint32 m_configuredStep{1};
    quint32 m_previousTimestamp{0};
    int m_intraFrameStride{1};
    bool m_havePrevious{false};
    int m_calibrationTransitions{0};
    QHash<quint32, int> m_stepHistogram;
    QVector<quint32> m_calibrationTimestamps;
};

class TimestampCheckerWorker : public QThread {
    Q_OBJECT

public:
    TimestampCheckerWorker(
        std::shared_ptr<ThreadSafeQueue<QByteArray>> inputQueue,
        std::atomic_bool *stopFlag,
        int durationMs,
        quint32 expectedStep,
        QObject *parent = nullptr);

signals:
    void progress(const ccv2::TimestampContinuityStats &stats, qint64 elapsedMs);
    void checkFinished(const ccv2::TimestampContinuityStats &stats,
                       qint64 elapsedMs,
                       bool durationReached);

protected:
    void run() override;

private:
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_inputQueue;
    std::atomic_bool *m_stopFlag{nullptr};
    int m_durationMs{0};
    quint32 m_expectedStep{1};
};

// Always-on background monitor: drains a hub-subscriber queue and reports
// continuity stats periodically, never stopping on duration. The intra-frame
// check is subsampled to keep it cheap. reset() is requested thread-safely
// (e.g. when a new acquisition timeline starts).
class TimestampMonitorWorker : public QThread {
    Q_OBJECT

public:
    TimestampMonitorWorker(
        std::shared_ptr<ThreadSafeQueue<QByteArray>> inputQueue,
        std::atomic_bool *stopFlag,
        quint32 expectedStep = 0,   // 0 = auto-calibrate
        int intraFrameStride = 64,
        QObject *parent = nullptr);

    void requestReset(quint32 expectedStep);

signals:
    void statsUpdated(const ccv2::TimestampContinuityStats &stats);

protected:
    void run() override;

private:
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_inputQueue;
    std::atomic_bool *m_stopFlag{nullptr};
    int m_intraFrameStride{64};
    std::atomic<quint32> m_expectedStep{0};
    std::atomic_bool m_resetPending{true};
};

}  // namespace ccv2

Q_DECLARE_METATYPE(ccv2::TimestampContinuityStats)
