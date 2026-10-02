#pragma once

#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

#include "core/frame_timestamp_reconciler.h"
#include "core/threadsafe_queue.h"
#include "signal/spike_filter.h"
#include "signal/spike_snippet_store.h"

namespace ccv2 {

struct SpikeDetectConfig {
    SpikeProcConfig proc;        // band filter + threshold detector
    double inputGain = 1.0;      // ADC output / gain = input-referred voltage
    int preSamples = 8;          // samples kept before the threshold crossing
    int postSamples = 24;        // samples kept after it
    bool tdmEnabled = false;     // one ADC channel interleaves four electrodes
    bool tdmPair02 = true;
};

// Detects spikes on all 256 channels (the selected 512-electrode pair in TDM mode) and stores
// threshold-aligned snippets. This runs off the GUI thread: filtering a full
// array at 20 kHz is far too heavy for a paint-timer callback.
//
// A crossing near the end of a chunk needs samples that have not arrived yet,
// so each lane keeps a small rolling history and a list of triggers waiting
// for their post-samples; snippets are emitted once the window is complete.
class SpikeDetectWorker : public QThread {
    Q_OBJECT

public:
    SpikeDetectWorker(std::shared_ptr<ThreadSafeQueue<QByteArray>> rawQueue,
                      SpikeSnippetStore *store,
                      std::atomic_bool *stopFlag,
                      const SpikeDetectConfig &cfg,
                      const std::atomic<quint64> *timelineEpoch = nullptr,
                      const std::atomic<int> *referenceMode = nullptr,
                      qint64 initialFrameIndex = 0,
                      QObject *parent = nullptr,
                      const std::atomic<qint64> *timelineFrameOrigin = nullptr);

    int laneCount() const { return m_lanes; }
    int snippetLength() const { return m_cfg.preSamples + m_cfg.postSamples + 1; }
    // Thread-safe: the GUI can change one lane without restarting acquisition
    // or rebuilding the detector worker.
    void setLaneThresholdOverride(int lane, bool enabled, double thresholdV);

signals:
    void framesProcessed(qint64 frames);

protected:
    void run() override;

private:
    struct LaneState {
        QVector<double> hist;      // filtered samples, lane-local
        qint64 histStart = 0;      // lane-sample index of hist[0]
        qint64 total = 0;          // lane samples ever seen
        QVector<qint64> pending;   // trigger indices awaiting post-samples
    };

    void resetLanes();
    void feedLane(int lane, const QVector<double> &samples);
    void publishStats();
    void applyThresholdOverrides();

    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    SpikeSnippetStore *m_store;
    std::atomic_bool *m_stopFlag;
    SpikeDetectConfig m_cfg;
    const std::atomic<quint64> *m_timelineEpoch{nullptr};
    const std::atomic<int> *m_referenceMode{nullptr};
    const std::atomic<qint64> *m_timelineFrameOrigin{nullptr};

    int m_lanes = 0;
    QByteArray m_leftover;
    quint64 m_lastEpoch{0};
    qint64 m_nextFrameIndex{0};
    FrameTimestampReconciler m_tsRecon;
    SpikeProcessor m_proc;
    QVector<LaneState> m_lane;
    QVector<float> m_snippetScratch;
    QVector<double> m_thresholdScratch;
    QVector<double> m_rmsScratch;
    QMutex m_thresholdOverrideLock;
    QVector<bool> m_thresholdOverrideEnabled;
    QVector<double> m_thresholdOverrideV;
    std::atomic<quint64> m_thresholdOverrideRevision{1};
    quint64 m_appliedThresholdOverrideRevision{0};
};

}  // namespace ccv2
