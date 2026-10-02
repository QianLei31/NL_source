#pragma once

#include <QByteArray>
#include <QMutex>
#include <QJsonObject>
#include <QUuid>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>
#include <functional>

#include "core/frame_timestamp_reconciler.h"
#include "core/threadsafe_queue.h"
#include "signal/spike_filter.h"
#include "signal/spike_snippet_store.h"
#include "signal/spike_rule_classifier.h"
#include "io/spike_event_archive.h"

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

    using RevisionCallback = std::function<void(quint64, quint64, const QJsonObject &)>;
    using EventCallback = std::function<void(const QVector<SpikeArchiveRecord> &)>;
    // Configure before first start. Callbacks execute synchronously on this
    // worker thread, must not throw, and must not wait for the GUI thread.
    // Every revision is registered before its first immutable event batch.
    bool setEventCallbacks(const QUuid &runId, RevisionCallback revision, EventCallback events);
    QUuid runId() const { return m_runId; }
    // Applied at the next processing flush; old pending crossings retain their
    // captured snapshot/context. Clear via a new nonzero empty rule revision.
    bool setRuleSet(SpikeRuleSet::Snapshot rules);
    SpikeRuleSet::Snapshot appliedRuleSet() const;
    // Optional revision is captured atomically with the context for GUI previews.
    SpikeRuleContext appliedRuleContext(int lane, quint64 *detectorRevision = nullptr) const;
    quint64 detectorRevision() const { return m_detectorRevision.load(); }
    // Producer must already be stopped. Unlike cancellation, drain consumes the
    // entire queue, publishes final stats, and excludes incomplete tail windows.
    void requestDrain();
    bool drained() const { return m_drained.load(); }


signals:
    void framesProcessed(qint64 frames);

protected:
    void run() override;

private:
    struct PendingCrossing {
        qint64 sample = 0;
        SpikeRuleSet::Snapshot rules;
        SpikeRuleContext context;
        quint64 detectorRevision = 0;
    };
    struct LaneState {
        QVector<double> hist;      // filtered samples, lane-local
        QVector<qint64> frames;   // exact source coordinate for each hist sample
        qint64 histStart = 0;      // lane-sample index of hist[0]
        qint64 total = 0;          // lane samples ever seen
        QVector<PendingCrossing> pending; // immutable crossing-time provenance
    };

    void resetLanes(bool accountDiscarded = true);
    void feedLane(int lane, const QVector<double> &samples, const QVector<qint64> &frames);
    void publishStats();
    void applyConfigurationBoundary(int referenceMode, qint64 firstSourceFrame,
                                    const char *boundarySemantics, bool forceDetectorRevision = false);
    SpikeRuleContext contextForLaneLocked(int lane) const;
    QJsonObject revisionMetadataLocked(qint64 firstSourceFrame, const char *boundarySemantics) const;
    void publishCompletedEvents();

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
    quint64 m_continuitySegment{0};
    qint64 m_nextFrameIndex{0};
    FrameTimestampReconciler m_tsRecon;
    SpikeProcessor m_proc;
    QVector<LaneState> m_lane;
    QVector<float> m_snippetScratch;
    QVector<double> m_thresholdScratch;
    QVector<double> m_rmsScratch;
    mutable QMutex m_configurationLock;
    QVector<bool> m_thresholdOverrideEnabled;
    QVector<double> m_thresholdOverrideV;
    quint64 m_thresholdOverrideRevision{1}; // protected by configuration lock
    quint64 m_appliedThresholdOverrideRevision{0};
    QVector<bool> m_appliedThresholdOverrideEnabled;
    QVector<double> m_appliedThresholdOverrideV;
    int m_appliedReferenceMode = 0;
    SpikeRuleSet::Snapshot m_requestedRules;
    SpikeRuleSet::Snapshot m_appliedRules;
    std::atomic<quint64> m_detectorRevision{1};
    quint64 m_announcedDetectorRevision = 0;
    quint64 m_announcedRuleRevision = 0;
    QUuid m_runId = QUuid::createUuid();
    quint64 m_nextEventId = 1;
    RevisionCallback m_revisionCallback;
    EventCallback m_eventCallback;
    QVector<SpikeArchiveRecord> m_completedEvents;
    std::atomic_bool m_started{false};
    std::atomic_bool m_drainRequested{false};
    std::atomic_bool m_drained{false};
};

}  // namespace ccv2
