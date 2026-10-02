#include "signal/spike_sorting_model.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace ccv2 {

double SpikeFeatures::value(SpikeFeatureAxis axis) const {
    switch (axis) {
    case SpikeFeatureAxis::PeakToPeak: return peakToPeakUv;
    case SpikeFeatureAxis::SignedPeak: return signedPeakUv;
    case SpikeFeatureAxis::Energy: return energyUv2Ms;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

SpikeFeatures SpikeSortingModel::extractFeatures(const SpikeEvent &event,
                                                 const float *samples, int length) {
    SpikeFeatures f;
    if (!samples || length <= 0 || length > kMaxSnippetSamples) return f;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo, peak = 0, energy = 0;
    for (int i = 0; i < length; ++i) {
        const double uv = double(samples[i]) * 1e6;
        if (!std::isfinite(uv)) return f;
        lo = std::min(lo, uv);
        hi = std::max(hi, uv);
        if (std::abs(uv) > std::abs(peak)) peak = uv;
        energy += uv * uv;
    }
    f.finiteWaveform = true;
    f.peakToPeakUv = hi - lo;
    f.signedPeakUv = peak;
    if (std::isfinite(event.sourceSampleRate) && event.sourceSampleRate > 0 &&
        event.sampleStride > 0) {
        f.energyUv2Ms = energy * 1000.0 * event.sampleStride / event.sourceSampleRate;
        if (!std::isfinite(f.energyUv2Ms))
            f.energyUv2Ms = std::numeric_limits<double>::quiet_NaN();
    }
    return f;
}

void SpikeSortingModel::clear() {
    m_lane = -1;
    m_length = 0;
    m_omitted = 0;
    m_events.clear();
    m_waveforms.clear();
    m_features.clear();
}

bool SpikeSortingModel::setSnapshot(int lane, const QVector<SpikeEvent> &events,
                                    const QVector<float> &waveforms, int length) {
    clear();
    if (lane < 0 || length < 0 || length > kMaxSnippetSamples ||
        (!events.isEmpty() && length == 0) ||
        qint64(events.size()) * length != waveforms.size()) return false;
    for (const auto &e : events) {
        if (e.lane != lane || e.epoch != events.first().epoch) return false;
    }
    m_lane = lane;
    m_length = length;
    if (events.isEmpty()) return true;
    const int cap = int(std::min<qint64>(kMaxEvents,
                      kWaveformBudgetBytes / (qint64(length) * sizeof(float))));
    m_omitted = std::max(0, int(events.size()) - cap);
    const int count = int(events.size()) - m_omitted;
    m_events.reserve(count);
    m_features.reserve(count);
    m_waveforms.reserve(count * length);
    for (int i = m_omitted; i < events.size(); ++i) {
        m_events.append(events[i]);
        m_features.append(extractFeatures(events[i], waveforms.constData() + i * length, length));
        for (int j = 0; j < length; ++j) m_waveforms.append(waveforms[i * length + j]);
    }
    return true;
}

const float *SpikeSortingModel::waveform(int row) const {
    if (row < 0 || row >= m_events.size() || m_length == 0) return nullptr;
    return m_waveforms.constData() + row * m_length;
}

QVector<int> SpikeSortingModel::rowsForUnit(int unitId) const {
    QVector<int> rows;
    for (int i = 0; i < m_events.size(); ++i)
        if (unitId == kAllUnits || m_events[i].unitId == unitId) rows.append(i);
    return rows;
}

QVector<int> SpikeSortingModel::selectRectangle(SpikeFeatureAxis x, SpikeFeatureAxis y,
                                              double x0, double x1, double y0, double y1,
                                              int unitId) const {
    QVector<int> rows;
    if (!std::isfinite(x0) || !std::isfinite(x1) ||
        !std::isfinite(y0) || !std::isfinite(y1)) return rows;
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int row : rowsForUnit(unitId)) {
        const double xv = m_features[row].value(x), yv = m_features[row].value(y);
        if (std::isfinite(xv) && std::isfinite(yv) && xv >= x0 && xv <= x1 &&
            yv >= y0 && yv <= y1) rows.append(row);
    }
    return rows;
}

SpikeUnitQc SpikeSortingModel::unitQc(int unitId, double shortIntervalMs) const {
    SpikeUnitQc qc;
    const QVector<int> rows = rowsForUnit(unitId);
    qc.retainedEvents = int(rows.size());
    QVector<int> timed;
    const SpikeEvent *reference = nullptr;
    QVector<double> sumSq;
    for (int row : rows) {
        const auto &e = m_events[row];
        if (e.hasSourceTime() && std::isfinite(e.sourceSampleRate)) {
            timed.append(row);
            const double t = e.sourceTimeSeconds();
            if (qc.firstSourceSeconds < 0 || t < qc.firstSourceSeconds) qc.firstSourceSeconds = t;
            qc.lastSourceSeconds = std::max(qc.lastSourceSeconds, t);
        } else ++qc.unknownTimes;
        if (!m_features[row].finiteWaveform) { ++qc.invalidWaveforms; continue; }
        if (!std::isfinite(e.sourceSampleRate) || e.sourceSampleRate <= 0 ||
            e.sampleStride <= 0 || e.preSamples < 0 || e.preSamples >= m_length) {
            ++qc.incompatibleTemplates;
            continue;
        }
        if (!reference) {
            reference = &e;
            qc.meanUv.fill(0, m_length);
            sumSq.fill(0, m_length);
            qc.preSamples = e.preSamples;
            qc.waveformStepMs = 1000.0 * e.sampleStride / e.sourceSampleRate;
        }
        if (e.sourceSampleRate != reference->sourceSampleRate ||
            e.sampleStride != reference->sampleStride || e.preSamples != reference->preSamples) {
            ++qc.incompatibleTemplates;
            continue;
        }
        ++qc.templateEvents;
        for (int j = 0; j < m_length; ++j) {
            const double uv = waveform(row)[j] * 1e6;
            qc.meanUv[j] += uv;
            sumSq[j] += uv * uv;
        }
    }
    if (qc.templateEvents) {
        qc.stddevUv.resize(m_length);
        for (int j = 0; j < m_length; ++j) {
            qc.meanUv[j] /= qc.templateEvents;
            qc.stddevUv[j] = std::sqrt(std::max(0.0,
                sumSq[j] / qc.templateEvents - qc.meanUv[j] * qc.meanUv[j]));
        }
    }
    // Source clocks from unrelated epochs/segments must never create an ISI.
    std::sort(timed.begin(), timed.end(), [this](int a, int b) {
        const auto &x = m_events[a]; const auto &y = m_events[b];
        return std::tie(x.epoch, x.continuitySegment, x.lane, x.sourceSampleRate, x.sourceFrame, x.sequence) <
               std::tie(y.epoch, y.continuitySegment, y.lane, y.sourceSampleRate, y.sourceFrame, y.sequence);
    });
    for (int i = 1; i < timed.size(); ++i) {
        const auto &a = m_events[timed[i - 1]], &b = m_events[timed[i]];
        if (a.epoch != b.epoch || a.continuitySegment != b.continuitySegment ||
            a.lane != b.lane || a.sourceSampleRate != b.sourceSampleRate) continue;
        const double ms = double(b.sourceFrame - a.sourceFrame) * 1000 / b.sourceSampleRate;
        qc.intervalsMs.append(ms);
        if (ms < shortIntervalMs) ++qc.shortIntervals;
    }
    return qc;
}

} // namespace ccv2
