#pragma once

#include <QVector>
#include <limits>

#include "signal/spike_event.h"

namespace ccv2 {

enum class SpikeFeatureAxis { PeakToPeak, SignedPeak, Energy };

struct SpikeFeatures {
    double peakToPeakUv = std::numeric_limits<double>::quiet_NaN();
    // Largest absolute sample, retaining its sign; ties use the first sample.
    double signedPeakUv = std::numeric_limits<double>::quiet_NaN();
    // Integral of squared input-referred voltage over the retained waveform.
    double energyUv2Ms = std::numeric_limits<double>::quiet_NaN();
    bool finiteWaveform = false;
    double value(SpikeFeatureAxis axis) const;
};

struct SpikeUnitQc {
    int retainedEvents = 0;
    int invalidWaveforms = 0;
    int unknownTimes = 0;
    int templateEvents = 0;
    int incompatibleTemplates = 0;
    int shortIntervals = 0;
    double firstSourceSeconds = -1;
    double lastSourceSeconds = -1;
    double waveformStepMs = 0;
    int preSamples = 0;
    QVector<double> meanUv;
    QVector<double> stddevUv;
    // Only adjacent events of this candidate within the same epoch, lane,
    // continuity segment and source sample rate contribute an interval.
    QVector<double> intervalsMs;
};

// GUI-thread value model of ONE bounded lane snapshot. This does no detection,
// automatic clustering, recording, or unbounded accumulation. Unit labels are
// annotations from the owning store, never inferred biological identities.
class SpikeSortingModel {
public:
    static constexpr int kAllUnits = -2;
    static constexpr int kMaxEvents = 2048;
    static constexpr qint64 kWaveformBudgetBytes = 16LL * 1024 * 1024;
    static constexpr int kMaxSnippetSamples = 8192;

    static SpikeFeatures extractFeatures(const SpikeEvent &event,
                                        const float *samples, int length);
    // Returns false and clears the model on malformed/mixed-lane snapshots.
    // Retains the newest rows if either explicit memory bound is exceeded.
    bool setSnapshot(int lane, const QVector<SpikeEvent> &events,
                     const QVector<float> &waveforms, int snippetLength);
    void clear();
    int lane() const { return m_lane; }
    int snippetLength() const { return m_length; }
    int omittedEvents() const { return m_omitted; }
    const QVector<SpikeEvent> &events() const { return m_events; }
    const QVector<SpikeFeatures> &features() const { return m_features; }
    const float *waveform(int row) const;
    QVector<int> rowsForUnit(int unitId = kAllUnits) const;
    QVector<int> selectRectangle(SpikeFeatureAxis x, SpikeFeatureAxis y,
                                 double x0, double x1, double y0, double y1,
                                 int unitId = kAllUnits) const;
    SpikeUnitQc unitQc(int unitId = kAllUnits,
                       double shortIntervalMs = 2.0) const;

private:
    int m_lane = -1;
    int m_length = 0;
    int m_omitted = 0;
    QVector<SpikeEvent> m_events;
    QVector<float> m_waveforms;
    QVector<SpikeFeatures> m_features;
};

} // namespace ccv2
