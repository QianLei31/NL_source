#include "signal/spike_snippet_store.h"

#include <algorithm>
#include <cmath>
#include <QSet>

namespace ccv2 {

int SpikeSnippetStore::boundedCapacity(int channels, int snippetLen, int requested) {
    if (channels <= 0 || snippetLen <= 0 || requested <= 0) return 0;
    const qint64 bytesPerEvent = static_cast<qint64>(snippetLen) * sizeof(float) + sizeof(SpikeEvent);
    const qint64 limit = kMemoryBudgetBytes / channels / bytesPerEvent;
    return static_cast<int>(qMin<qint64>(requested, limit));
}

bool SpikeSnippetStore::configure(int channels, int snippetLen, int capacityPerChannel) {
    const int capacity = boundedCapacity(channels, snippetLen, capacityPerChannel);
    if (capacity <= 0) return false;
    std::unique_ptr<float[]> ring;
    std::unique_ptr<SpikeEvent[]> events;
    try {
        // Do not zero hundreds of MB on the GUI thread. Only slots that have
        // been completely written are exposed through m_count.
        ring.reset(new float[static_cast<qsizetype>(channels) * capacity * snippetLen]);
        events.reset(new SpikeEvent[static_cast<qsizetype>(channels) * capacity]);
    } catch (const std::bad_alloc &) {
        return false;
    }
    QMutexLocker locker(&m_lock);
    m_channels = qMax(0, channels);
    m_len = qMax(1, snippetLen);
    m_cap = capacity;
    m_ring = std::move(ring);
    m_events = std::move(events);
    m_head.fill(0, m_channels);
    m_count.fill(0, m_channels);
    // Preserve identities across detector reconfiguration, including removed
    // and later re-added lanes. Frozen candidate selections must never hit a
    // new waveform with the same (epoch,lane,sequence).
    m_seq.resize(qMax(m_seq.size(), qsizetype(m_channels)));
    m_total.fill(0, m_channels);
    m_observedSamples.fill(0, m_channels);
    m_pendingWindowCounts.fill(0, m_channels);
    m_quality = {};
    m_quality.epoch = m_epoch;
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
    std::fill(m_observedSamples.begin(), m_observedSamples.end(), 0);
    std::fill(m_pendingWindowCounts.begin(), m_pendingWindowCounts.end(), 0);
    m_quality = {};
    m_quality.epoch = m_epoch;
    // m_seq is deliberately NOT reset: consumers compare against their own
    // stored sequence, and rewinding it would make already-drawn snippets look
    // new. fetchNew clamps to m_count, so a cleared ring yields nothing.
}

void SpikeSnippetStore::addSnippet(int channel, const float *samples) {
    if (!samples) return;
    QMutexLocker locker(&m_lock);
    SpikeEvent event;
    event.epoch = m_epoch;
    addSnippetLocked(channel, samples, event);
}

bool SpikeSnippetStore::addSnippetIfEpoch(int channel, const float *samples, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch || !samples) return false;
    SpikeEvent event;
    event.epoch = epoch;
    return addSnippetLocked(channel, samples, event);
}

bool SpikeSnippetStore::addEventIfEpoch(int channel, const float *samples, const SpikeEvent &event) {
    QMutexLocker locker(&m_lock);
    if (event.epoch != m_epoch || !samples || event.sourceFrame < 0 ||
        !std::isfinite(event.sourceSampleRate) || event.sourceSampleRate <= 0.0 ||
        !std::isfinite(event.inputGain) || event.inputGain <= 0.0 ||
        event.preSamples < 0 || event.preSamples >= m_len ||
        (event.sampleStride != 1 && event.sampleStride != 4) ||
        event.adcChannel < 0 || event.adcChannel >= 256 ||
        (event.sampleStride == 1 && (event.tdmPhase != -1 || event.electrode != event.adcChannel)) ||
        (event.sampleStride == 4 && (event.tdmPhase < 0 || event.tdmPhase > 3 ||
          event.sourceFrame % 4 != event.tdmPhase || event.electrode != event.adcChannel * 4 + event.tdmPhase))) {
        return false;
    }
    return addSnippetLocked(channel, samples, event);
}

bool SpikeSnippetStore::addSnippetLocked(int channel, const float *samples, SpikeEvent event) {
    if (channel < 0 || channel >= m_channels || !m_ring || !m_events) return false;
    const qsizetype slot =
        (static_cast<qsizetype>(channel) * m_cap + m_head[channel]) * m_len;
    std::copy(samples, samples + m_len, m_ring.get() + slot);
    event.lane = channel;
    event.sequence = m_seq[channel] + 1;
    m_events[static_cast<qsizetype>(channel) * m_cap + m_head[channel]] = event;
    m_head[channel] = (m_head[channel] + 1) % m_cap;
    if (m_count[channel] < m_cap) ++m_count[channel];
    ++m_seq[channel];
    ++m_total[channel];
    return true;
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

int SpikeSnippetStore::fetchEvents(int channel, QVector<SpikeEvent> *events,
                                   QVector<float> *waveforms, quint64 *seqOut) const {
    if (!events) return 0;
    QMutexLocker locker(&m_lock);
    events->clear();
    if (waveforms) waveforms->clear();
    if (seqOut) *seqOut = 0;
    if (channel < 0 || channel >= m_channels) return 0;
    const int have = m_count[channel];
    if (seqOut) *seqOut = m_seq[channel];
    events->resize(have);
    if (waveforms) waveforms->resize(static_cast<qsizetype>(have) * m_len);
    const qsizetype chBase = static_cast<qsizetype>(channel) * m_cap;
    for (int i = 0; i < have; ++i) {
        const int slot = ((m_head[channel] - have + i) % m_cap + m_cap) % m_cap;
        (*events)[i] = m_events[chBase + slot];
        if (waveforms) {
            const auto src = m_ring.get() + (chBase + slot) * m_len;
            std::copy(src, src + m_len, waveforms->begin() + static_cast<qsizetype>(i) * m_len);
        }
    }
    return have;
}

SpikeLaneSnapshot SpikeSnippetStore::snapshotLane(int channel) const {
    QMutexLocker locker(&m_lock);
    SpikeLaneSnapshot snapshot;
    if (channel < 0 || channel >= m_channels) return snapshot;
    snapshot.lane = channel;
    snapshot.snippetLength = m_len;
    snapshot.capacity = m_cap;
    snapshot.sequence = m_seq[channel];
    snapshot.totalDetected = m_total[channel];
    snapshot.observedSamples = m_observedSamples[channel];
    snapshot.thresholdV = m_threshold[channel];
    snapshot.noiseRmsV = m_rms[channel];
    snapshot.quality = m_quality;
    const int have = m_count[channel];
    snapshot.events.resize(have);
    snapshot.waveforms.resize(static_cast<qsizetype>(have) * m_len);
    const qsizetype base = static_cast<qsizetype>(channel) * m_cap;
    for (int i = 0; i < have; ++i) {
        const int slot = ((m_head[channel] - have + i) % m_cap + m_cap) % m_cap;
        snapshot.events[i] = m_events[base + slot];
        const float *src = m_ring.get() + (base + slot) * m_len;
        std::copy(src, src + m_len, snapshot.waveforms.begin() + static_cast<qsizetype>(i) * m_len);
    }
    return snapshot;
}

void SpikeSnippetStore::setAnnotationCallback(std::function<void(const QVector<SpikeEvent> &)> callback) {
    QMutexLocker locker(&m_lock); m_annotationCallback = std::move(callback);
}

int SpikeSnippetStore::setCandidateUnit(quint64 epoch, int lane,
                                        const QVector<quint64> &sequences, int unitId) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch || lane < 0 || lane >= m_channels || unitId < -1) return 0;
    const QSet<quint64> selected(sequences.cbegin(), sequences.cend());
    QVector<SpikeEvent> changedEvents;
    int changed = 0;
    const qsizetype base = static_cast<qsizetype>(lane) * m_cap;
    for (int i = 0; i < m_count[lane]; ++i) {
        const int slot = ((m_head[lane] - m_count[lane] + i) % m_cap + m_cap) % m_cap;
        SpikeEvent &event = m_events[base + slot];
        if (event.epoch == epoch && selected.contains(event.sequence)) {
            event.unitId = unitId;
            event.classification = SpikeClassificationStatus::Manual;
            changedEvents.push_back(event);
            ++changed;
        }
    }
    const auto callback = m_annotationCallback;
    locker.unlock();
    if (callback && !changedEvents.isEmpty()) callback(changedEvents);
    return changed;
}

void SpikeSnippetStore::addCoverageIfEpoch(int channel, qint64 samples, qint64 firstFrame,
                                           qint64 lastFrame, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch || channel < 0 || channel >= m_channels || samples <= 0 ||
        firstFrame < 0 || lastFrame < firstFrame) return;
    m_observedSamples[channel] += samples;
    if (m_quality.firstSourceFrame < 0 || firstFrame < m_quality.firstSourceFrame)
        m_quality.firstSourceFrame = firstFrame;
    m_quality.lastSourceFrame = qMax(m_quality.lastSourceFrame, lastFrame);
}

void SpikeSnippetStore::markDiscontinuityIfEpoch(qint64 missingFrames, qint64 queueDroppedFrames,
                                                qint64 invalidFrames, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch) return;
    ++m_quality.discontinuities;
    m_quality.missingSourceFrames += qMax<qint64>(0, missingFrames);
    m_quality.queueDroppedFrames += qMax<qint64>(0, queueDroppedFrames);
    m_quality.invalidFrames += qMax<qint64>(0, invalidFrames);
}

void SpikeSnippetStore::setPendingWindowCountIfEpoch(int lane, qint64 count, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch != m_epoch || lane < 0 || lane >= m_channels) return;
    count = qMax<qint64>(0, count);
    m_quality.pendingWindowEvents += count - m_pendingWindowCounts[lane];
    m_pendingWindowCounts[lane] = count;
}

void SpikeSnippetStore::addBoundaryExcludedIfEpoch(qint64 count, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch == m_epoch) m_quality.boundaryExcludedEvents += qMax<qint64>(0, count);
}

void SpikeSnippetStore::markUnverifiedFramesIfEpoch(qint64 frames, quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch == m_epoch) m_quality.unverifiedFrames += qMax<qint64>(0, frames);
}

void SpikeSnippetStore::markAnalysisIncomplete(quint64 epoch) {
    QMutexLocker locker(&m_lock);
    if (epoch == m_epoch) m_quality.stoppedEarly = true;
}

void SpikeSnippetStore::snapshotAnalysis(QVector<qint64> *observedSamples,
                                         SpikeAnalysisQuality *quality,
                                         QVector<qint64> *totals,
                                         QVector<double> *thresholds,
                                         QVector<double> *rms) const {
    QMutexLocker locker(&m_lock);
    if (observedSamples) *observedSamples = m_observedSamples;
    if (quality) *quality = m_quality;
    if (totals) *totals = m_total;
    if (thresholds) *thresholds = m_threshold;
    if (rms) *rms = m_rms;
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
