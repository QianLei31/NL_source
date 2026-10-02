#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <memory>

#include "signal/spike_event.h"

namespace ccv2 {

// Rectangle coordinates are input-referred microvolts and milliseconds relative
// to the causal threshold-crossing sample. Edges are inclusive; area is positive.
struct SpikeWaveformBox {
    double timeMinMs = 0.0;
    double timeMaxMs = 0.0;
    double voltageMinUv = 0.0;
    double voltageMaxUv = 0.0;
};

struct SpikeCandidateRule {
    int unitId = 1; // stable candidate ID, 1..32, not biological neuron identity
    QVector<SpikeWaveformBox> boxes; // AND; each box must intersect the waveform
};

// All effective configured parameters affecting detection/waveform semantics.
// Values describe the crossing's configuration, not the current GUI controls.
// sourceSampleRate is before demultiplexing; waveform sampling is fs / stride.
struct SpikeRuleContext {
    int adcChannel = -1;
    int electrode = -1;
    int tdmPhase = -1; // ADC: -1/stride 1/electrode==adc; TDM: 0..3/stride 4
    int sampleStride = 1;
    double sourceSampleRate = 20000.0;
    double inputGain = 1.0;
    int referenceMode = 0; // 0 off, 1 common average, 2 median
    int filterOrder = 2;
    int notchHz = 50;
    double highpassHz = 250.0;
    double spikeLowpassHz = 6000.0;
    bool absoluteThreshold = false;
    double absThresholdV = -0.05;
    double rmsMultiple = 4.0;
    bool negativePolarity = true;
    double refractoryMs = 1.0;
    bool thresholdOverrideEnabled = false;
    double thresholdOverrideV = 0.0;
    QString alignment = QStringLiteral("causal_threshold_crossing_not_peak");
    int preSamples = 8;
    int postSamples = 24;
};

struct SpikeElectrodeRules {
    SpikeRuleContext context;
    QVector<SpikeCandidateRule> candidates;
};

struct SpikeRuleClassification {
    SpikeClassificationStatus status = SpikeClassificationStatus::Unassigned;
    int unitId = -1;
    QVector<int> matchingUnitIds; // sorted; >1 means ambiguous, never first-wins
    quint64 ruleRevision = 0;
    QString reason;
};

// Immutable, bounded QtCore-only rule snapshots. A pending crossing must retain
// the shared_ptr AND its context, so later edits cannot relabel that crossing.
class SpikeRuleSet final {
public:
    using Snapshot = std::shared_ptr<const SpikeRuleSet>;
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMaxElectrodes = 1024;
    static constexpr int kMaxUnits = 32;
    static constexpr int kMaxBoxesPerUnit = 16;
    static constexpr int kMaxTotalBoxes = 8192;
    static constexpr int kMaxSnippetSamples = 8192;
    static constexpr int kMaxJsonBytes = 4 * 1024 * 1024;

    static Snapshot create(quint64 revision, const QVector<SpikeElectrodeRules> &electrodes,
                           QString *error = nullptr);
    static Snapshot fromJson(const QByteArray &json, QString *error = nullptr);
    static Snapshot load(const QString &path, QString *error = nullptr);
    QByteArray toJson() const;
    bool save(const QString &path, QString *error = nullptr) const;

    quint64 revision() const { return m_revision; }
    const QVector<SpikeElectrodeRules> &electrodes() const { return m_electrodes; }
    // Empty fingerprint means invalid context. Exact SHA-256 compatibility,
    // including calibration, mode/physical electrode, filter, and threshold.
    static QByteArray compatibilityFingerprint(const SpikeRuleContext &context);
    static bool validateContext(const SpikeRuleContext &context, QString *error = nullptr);
    static bool validateBox(const SpikeWaveformBox &box, QString *error = nullptr);
    static bool waveformIntersectsBox(const SpikeWaveformBox &box,
                                      const SpikeEvent &event,
                                      const float *inputReferredVolts, int length);

    SpikeRuleClassification classify(const SpikeEvent &event,
                                     const float *inputReferredVolts, int length,
                                     const SpikeRuleContext &crossingContext) const;

private:
    SpikeRuleSet(quint64 revision, QVector<SpikeElectrodeRules> electrodes);
    quint64 m_revision;
    QVector<SpikeElectrodeRules> m_electrodes;
    QVector<QByteArray> m_fingerprints;
};

} // namespace ccv2
