#pragma once

#include <QMutex>
#include <QVector>
#include <memory>

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
    qint64 totalSpikes(int channel) const;
    double threshold(int channel) const;
    // Per-channel totals + thresholds + noise RMS in one locked pass, so a
    // 512-cell refresh does not take the mutex hundreds of times.
    void snapshotStats(QVector<qint64> *totals,
                       QVector<double> *thresholds,
                       QVector<double> *rms) const;

private:
    void addSnippetLocked(int channel, const float *samples);
    void clearLocked();
    mutable QMutex m_lock;
    quint64 m_epoch{0};
    int m_channels = 0;
    int m_len = 0;
    int m_cap = 0;
    std::unique_ptr<float[]> m_ring;  // unwritten slots are never read
    QVector<int> m_head;      // next slot to write, per channel
    QVector<int> m_count;     // retained snippets, per channel
    QVector<quint64> m_seq;   // snippets ever added, per channel (monotonic)
    QVector<qint64> m_total;  // spikes since the last clear(), per channel
    QVector<double> m_threshold;
    QVector<double> m_rms;
};

}  // namespace ccv2
