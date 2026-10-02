#pragma once

#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

#include "core/frame_timestamp_reconciler.h"
#include "core/threadsafe_queue.h"

namespace ccv2 {

struct RealtimeChannelMetric {
    double mean{0.0};
    double rms{0.0};
    double p2p{0.0};
    bool saturated{false};
    bool packetLoss{false};
    bool valid{false};
};

struct RealtimeStreamState {
    QMutex lock;
    QVector<int> channels;
    QMap<int, QQueue<double>> buffers;
    QMap<int, qint64> totalSamples;
    QVector<RealtimeChannelMetric> metrics;
    QVector<RealtimeChannelMetric> tdmMetrics; // size = 1024, channel * 4 + physical phase
    quint64 timelineEpoch{0};
    quint64 metricsEpoch{0};
    quint64 droppedFrames{0};
    int maxSamples{1200};
    int metricWindowFrames{2000};
};

class DataSorter : public QThread {
    Q_OBJECT

public:
    // timelineEpoch (optional): shared generation counter bumped by the session
    // hub on every seek/reset. Chunks popped before a bump are dropped at
    // commit time instead of contaminating the new timeline, which lets seeks
    // reset the display without tearing this thread down.
    // referenceMode (optional): shared 0=off/1=CAR/2=median software reference,
    // subtracted per-frame across all channels to remove array common-mode.
    DataSorter(std::shared_ptr<ThreadSafeQueue<QByteArray>> rawQueue,
               std::shared_ptr<RealtimeStreamState> state,
               std::atomic_bool *stopFlag,
               const std::atomic<quint64> *timelineEpoch = nullptr,
               const std::atomic<int> *referenceMode = nullptr,
               const std::atomic<qint64> *timelineFrameOrigin = nullptr,
               qint64 initialFrameIndex = 0,
               QObject *parent = nullptr);

signals:
    void framesParsed(int frameCount, int leftoverBytes);

protected:
    void run() override;

private:
    void resetMetricWindow();

    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    std::shared_ptr<RealtimeStreamState> m_state;
    std::atomic_bool *m_stopFlag;
    const std::atomic<quint64> *m_timelineEpoch{nullptr};
    const std::atomic<int> *m_referenceMode{nullptr};
    const std::atomic<qint64> *m_timelineFrameOrigin{nullptr};
    QByteArray m_leftover;
    quint64 m_lastDroppedFrames{0};
    quint64 m_lastEpoch{0};
    qint64 m_nextFrameIndex{0};
    // Recovers the true frame index from the hardware timestamp so upstream
    // frame loss advances the count (keeping TDM parity) instead of silently
    // shifting it. Dormant on zero-timestamp streams.
    FrameTimestampReconciler m_tsRecon;
    int m_lastReferenceMode{0};
    int m_metricFrameCount{0};
    bool m_metricPacketLoss{false};
    QVector<double> m_metricSum;
    QVector<double> m_metricSumSq;
    QVector<double> m_metricMin;
    QVector<double> m_metricMax;
    QVector<double> m_metricOrigSum;
    QVector<double> m_metricOrigMin;
    QVector<double> m_metricOrigMax;
    QVector<double> m_tdmSum;
    QVector<double> m_tdmSumSq;
    QVector<double> m_tdmMin;
    QVector<double> m_tdmMax;
    QVector<double> m_tdmOrigSum;
    QVector<double> m_tdmOrigMin;
    QVector<double> m_tdmOrigMax;
    QVector<int> m_tdmCount;
};

}  // namespace ccv2
