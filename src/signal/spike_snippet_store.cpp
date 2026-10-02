#include "signal/spike_snippet_store.h"

#include <algorithm>

namespace ccv2 {

int SpikeSnippetStore::boundedCapacity(int channels, int snippetLen, int requested) {
    if (channels <= 0 || snippetLen <= 0 || requested <= 0) return 0;
    const qint64 limit = kMemoryBudgetBytes / sizeof(float) / channels / snippetLen;
    return static_cast<int>(qMin<qint64>(requested, limit));
}

bool SpikeSnippetStore::configure(int channels, int snippetLen, int capacityPerChannel) {
    const int capacity = boundedCapacity(channels, snippetLen, capacityPerChannel);
    if (capacity <= 0) return false;
    std::unique_ptr<float[]> ring;
    try {
        // Do not zero hundreds of MB on the GUI thread. Only slots that have
        // been completely written are exposed through m_count.
        ring.reset(new float[static_cast<qsizetype>(channels) * capacity * snippetLen]);
    } catch (const std::bad_alloc &) {
        return false;
    }
    QMutexLocker locker(&m_lock);
    m_channels = qMax(0, channels);
    m_len = qMax(1, snippetLen);
    m_cap = capacity;
    m_ring = std::move(ring);
    m_head.fill(0, m_channels);
    m_count.fill(0, m_channels);
    m_seq.fill(0, m_channels);
    m_total.fill(0, m_channels);
    m_threshold.fill(0.0, m_channels);
    m_rms.fill(0.0, m_channels);
    return true;
}

void SpikeSnippetStore::clear() {
    QMutexLocker locker(&m_lock);
    clearLocked();
}

void SpikeSnippetStore::resetTimeline(quint64 epoch) {
    QMutexLocker locker(&m_lock);
    m_epoch = epoch;
    clearLocked();
    m_threshold.fill(0.0);
    m_rms.fill(0.0);
}

void SpikeSnippetStore::clearLocked() {
    std::fill(m_head.begin(), m_head.end(), 0);
    std::fill(m_count.begin(), m_count.end(), 0);
    std::fill(m_total.begin(), m_total.end(), 0);
    // m_seq is deliberately NOT reset: consumers compare against their own
    // stored sequence, and rewinding it would make already-drawn snippets look
    // new. fetchNew clamps to m_count, so a cleared ring yields nothing.
}

void SpikeSnippetStore::addSnippet(int channel, const float *samples) {
    if (!samples) return;
    QMutexLocker locker(&m_lock);
    addSnippetLocked(channel, samples);
}

bool SpikeSnippetStore::addSnippetIfEpoch(int channel, const float *samples, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch || !samples) return false;
    addSnippetLocked(channel, samples);
    return true;
}

void SpikeSnippetStore::addSnippetLocked(int channel, const float *samples) {
    if (channel < 0 || channel >= m_channels) return;
    const qsizetype slot =
        (static_cast<qsizetype>(channel) * m_cap + m_head[channel]) * m_len;
    std::copy(samples, samples + m_len, m_ring.get() + slot);
    m_head[channel] = (m_head[channel] + 1) % m_cap;
    if (m_count[channel] < m_cap) ++m_count[channel];
    ++m_seq[channel];
    ++m_total[channel];
}

void SpikeSnippetStore::setStats(const QVector<double> &thresholds, const QVector<double> &rms) {
    QMutexLocker locker(&m_lock);
    const int n = qMin(m_channels, qMin(thresholds.size(), rms.size()));
    for (int i = 0; i < n; ++i) {
        m_threshold[i] = thresholds[i];
        m_rms[i] = rms[i];
    }
}

void SpikeSnippetStore::setStatsIfEpoch(const QVector<double> &thresholds, const QVector<double> &rms, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch) return;
    const int n = qMin(m_channels, qMin(thresholds.size(), rms.size()));
    for (int i = 0; i < n; ++i) { m_threshold[i] = thresholds[i]; m_rms[i] = rms[i]; }
}

int SpikeSnippetStore::channels() const {
    QMutexLocker locker(&m_lock);
    return m_channels;
}

int SpikeSnippetStore::snippetLength() const {
    QMutexLocker locker(&m_lock);
    return m_len;
}

int SpikeSnippetStore::capacity() const {
    QMutexLocker locker(&m_lock);
    return m_cap;
}

int SpikeSnippetStore::fetchNew(int channel, quint64 *sinceSeq,
                                QVector<float> *flatOut, int maxCount) const {
    if (!flatOut || !sinceSeq) return 0;
    QMutexLocker locker(&m_lock);
    if (channel < 0 || channel >= m_channels) return 0;
    const quint64 seq = m_seq[channel];
    if (seq <= *sinceSeq) {
        *sinceSeq = seq;
        return 0;
    }
    int fresh = static_cast<int>(
        std::min<quint64>(seq - *sinceSeq, static_cast<quint64>(m_count[channel])));
    if (maxCount > 0) {
        fresh = qMin(fresh, maxCount);
    }
    *sinceSeq = seq;
    if (fresh <= 0) return 0;

    flatOut->resize(static_cast<qsizetype>(fresh) * m_len);
    const qsizetype chBase = static_cast<qsizetype>(channel) * m_cap;
    for (int i = 0; i < fresh; ++i) {
        // Oldest-first walk over the newest `fresh` slots behind the head.
        const int slot = ((m_head[channel] - fresh + i) % m_cap + m_cap) % m_cap;
        const auto src = m_ring.get() + (chBase + slot) * m_len;
        std::copy(src, src + m_len, flatOut->begin() + static_cast<qsizetype>(i) * m_len);
    }
    return fresh;
}

int SpikeSnippetStore::fetchAll(int channel, QVector<float> *flatOut, quint64 *seqOut) const {
    if (!flatOut) return 0;
    QMutexLocker locker(&m_lock);
    if (channel < 0 || channel >= m_channels) return 0;
    const int have = m_count[channel];
    if (seqOut) *seqOut = m_seq[channel];
    flatOut->resize(static_cast<qsizetype>(have) * m_len);
    if (have <= 0) return 0;
    const qsizetype chBase = static_cast<qsizetype>(channel) * m_cap;
    for (int i = 0; i < have; ++i) {
        const int slot = ((m_head[channel] - have + i) % m_cap + m_cap) % m_cap;
        const auto src = m_ring.get() + (chBase + slot) * m_len;
        std::copy(src, src + m_len, flatOut->begin() + static_cast<qsizetype>(i) * m_len);
    }
    return have;
}

qint64 SpikeSnippetStore::totalSpikes(int channel) const {
    QMutexLocker locker(&m_lock);
    if (channel < 0 || channel >= m_channels) return 0;
    return m_total[channel];
}

double SpikeSnippetStore::threshold(int channel) const {
    QMutexLocker locker(&m_lock);
    if (channel < 0 || channel >= m_channels) return 0.0;
    return m_threshold[channel];
}

void SpikeSnippetStore::snapshotStats(QVector<qint64> *totals,
                                      QVector<double> *thresholds,
                                      QVector<double> *rms) const {
    QMutexLocker locker(&m_lock);
    if (totals) *totals = m_total;
    if (thresholds) *thresholds = m_threshold;
    if (rms) *rms = m_rms;
}

}  // namespace ccv2
