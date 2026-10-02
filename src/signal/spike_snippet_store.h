#pragma once

#include <QMutex>
#include <QVector>
#include <memory>

#include "signal/spike_event.h"

namespace ccv2 {

// Ring buffer of threshold-aligned spike snippets, one ring per channel
// (Blackrock Central's spike panel keeps the last N waveforms per electrode so
// unit activity is visible as a persistent overlay rather than a passing
// sweep). The detection worker writes; the GUI reads on its refresh timer.
//
// Consumers hold a per-channel sequence number and ask for whatever arrived
// since then, so the view draws each snippet exactly once and never has to
// re-render the whole ring on a normal refresh. Falling more than `capacity`
// behind simply yields the newest `capacity` snippets.
struct SpikeAnalysisQuality {
    quint64 epoch = 0;
    qint64 missingSourceFrames = 0;
    qint64 queueDroppedFrames = 0;
    qint64 invalidFrames = 0;
    qint64 pendingWindowEvents = 0;
    qint64 boundaryExcludedEvents = 0;
    qint64 unverifiedFrames = 0; // no persisted/source validity mask supplied
    qint64 discontinuities = 0;
    bool stoppedEarly = false;
    qint64 firstSourceFrame = -1;
    qint64 lastSourceFrame = -1;
    bool incomplete() const {
        return missingSourceFrames > 0 || queueDroppedFrames > 0 ||
               invalidFrames > 0 || unverifiedFrames > 0 || pendingWindowEvents > 0 ||
               boundaryExcludedEvents > 0 || discontinuities > 0 || stoppedEarly;
    }
};

struct SpikeLaneSnapshot {
    int lane = -1;
    int snippetLength = 0;
    int capacity = 0;
    quint64 sequence = 0;
    qint64 totalDetected = 0;
    qint64 observedSamples = 0;
    double thresholdV = 0.0;
    double noiseRmsV = 0.0;
    SpikeAnalysisQuality quality;
    QVector<SpikeEvent> events;
    QVector<float> waveforms;
};

class SpikeSnippetStore {
public:
    static constexpr qint64 kMemoryBudgetBytes = 128LL * 1024 * 1024;
    static int boundedCapacity(int channels, int snippetLen, int requested);
    bool configure(int channels, int snippetLen, int capacityPerChannel);
    void clear();
    void resetTimeline(quint64 epoch);

    // --- detection worker side ---
    void addSnippet(int channel, const float *samples);
    bool addSnippetIfEpoch(int channel, const float *samples, quint64 epoch);
    // Waveform and metadata are inserted/fetched under the same lock. Returns
    // false for an invalid lane, stale epoch or invalid source coordinates.
    bool addEventIfEpoch(int channel, const float *samples, const SpikeEvent &event);
    // Publish every channel's detector state in one locked pass (called a few
    // times per second, not per chunk).
    void setStats(const QVector<double> &thresholds, const QVector<double> &rms);
    void setStatsIfEpoch(const QVector<double> &thresholds, const QVector<double> &rms, quint64 epoch);

    // --- GUI side ---
    int channels() const;
    int snippetLength() const;
    int capacity() const;
    // Snippets added to `channel` after *sinceSeq, oldest first, flattened as
    // count * snippetLength floats. *sinceSeq is advanced to the current
    // sequence. Returns the number of snippets written.
    // Returns the newest snippets since `sinceSeq`. When maxCount is positive,
    // older pending snippets are skipped and the watermark still advances to
    // the current sequence, providing display-side backpressure.
    int fetchNew(int channel, quint64 *sinceSeq, QVector<float> *flatOut,
                 int maxCount = 0) const;
    // Every retained snippet, oldest first (used for full redraws on resize).
    int fetchAll(int channel, QVector<float> *flatOut, quint64 *seqOut) const;
    // Atomic metadata+waveform snapshot, oldest first. waveforms may be null
    // when only event times are needed. Metadata and waveform row i always match.
    int fetchEvents(int channel, QVector<SpikeEvent> *events,
                    QVector<float> *waveforms = nullptr, quint64 *seqOut = nullptr) const;
    SpikeLaneSnapshot snapshotLane(int channel) const;
    // Manual candidate labels only, scoped to retained event identity. Evicted
    // or stale-epoch selections cannot label a newer waveform in the same slot.
    int setCandidateUnit(quint64 epoch, int lane, const QVector<quint64> &sequences, int unitId);
    // Exposure is the number of samples actually processed in each lane,
    // independent of playback speed, UI visibility or display pause.
    void addCoverageIfEpoch(int channel, qint64 samples, qint64 firstFrame,
                            qint64 lastFrame, quint64 epoch);
    void markDiscontinuityIfEpoch(qint64 missingFrames, qint64 queueDroppedFrames,
                                 qint64 invalidFrames, quint64 epoch);
    void setPendingWindowCountIfEpoch(int lane, qint64 count, quint64 epoch);
    void addBoundaryExcludedIfEpoch(qint64 count, quint64 epoch);
    void markAnalysisIncomplete(quint64 epoch);
    void markUnverifiedFramesIfEpoch(qint64 frames, quint64 epoch);
    void snapshotAnalysis(QVector<qint64> *observedSamples,
                          SpikeAnalysisQuality *quality,
                          QVector<qint64> *totals = nullptr,
                          QVector<double> *thresholds = nullptr,
                          QVector<double> *rms = nullptr) const;
    qint64 totalSpikes(int channel) const;
    double threshold(int channel) const;
    // Per-channel totals + thresholds + noise RMS in one locked pass, so a
    // 512-cell refresh does not take the mutex hundreds of times.
    void snapshotStats(QVector<qint64> *totals,
                       QVector<double> *thresholds,
                       QVector<double> *rms) const;

private:
    bool addSnippetLocked(int channel, const float *samples, SpikeEvent event);
    void clearLocked();
    mutable QMutex m_lock;
    quint64 m_epoch{0};
    int m_channels = 0;
    int m_len = 0;
    int m_cap = 0;
    std::unique_ptr<float[]> m_ring;  // unwritten slots are never read
    std::unique_ptr<SpikeEvent[]> m_events;
    QVector<int> m_head;      // next slot to write, per channel
    QVector<int> m_count;     // retained snippets, per channel
    QVector<quint64> m_seq;   // snippets ever added, per channel (monotonic)
    QVector<qint64> m_observedSamples;
    QVector<qint64> m_pendingWindowCounts;
    SpikeAnalysisQuality m_quality;
    QVector<qint64> m_total;  // spikes since the last clear(), per channel
    QVector<double> m_threshold;
    QVector<double> m_rms;
};

}  // namespace ccv2
