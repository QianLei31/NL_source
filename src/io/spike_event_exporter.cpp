#include "io/spike_event_exporter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

namespace ccv2 {
QJsonObject spikeAnalysisMetadata(const SpikeDetectConfig &c, int referenceMode,
                                  const QMap<int, double> &overrides) {
    const auto &p = c.proc;
    QJsonObject o;
    o["schema_version"] = 1;
    o["algorithm"] = "nl-causal-butterworth-threshold-events-v1";
    o["waveform_units"] = "input_referred_volts";
    o["waveform_band"] = "spike";
    o["gain_provenance"] = "manual_uniform_not_hardware_verified";
    o["input_gain"] = c.inputGain;
    o["reference_mode"] = referenceMode;
    o["lane_sample_rate_hz"] = p.sampleRate;
    o["source_sample_rate_hz"] = p.sampleRate * (c.tdmEnabled ? 4 : 1);
    o["tdm_enabled"] = c.tdmEnabled;
    o["tdm_pair_02"] = c.tdmPair02;
    o["analysis_scope"] = c.tdmEnabled ? "selected_512_of_1024_electrodes" : "all_256_adc_channels";
    o["filter_order"] = p.filterOrder;
    o["notch_hz"] = p.notchHz;
    o["highpass_hz"] = p.highpassHz;
    o["spike_lowpass_hz"] = p.spikeLowpassHz;
    o["threshold_absolute"] = p.absoluteThreshold;
    o["threshold_volts"] = p.absThresholdV;
    o["rms_multiple"] = p.rmsMultiple;
    o["negative_polarity"] = p.negativePolarity;
    o["refractory_ms"] = p.refractoryMs;
    o["pre_samples"] = c.preSamples;
    o["post_samples"] = c.postSamples;
    o["alignment"] = "causal_threshold_crossing_not_peak";
    QJsonObject thresholdMap;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        thresholdMap[QString::number(it.key())] = it.value();
    o["threshold_overrides_uv_by_electrode"] = thresholdMap;
    return o;
}

bool exportSpikeLaneJson(const QString &path, const SpikeLaneSnapshot &s,
                         const QJsonObject &metadata, QString *error) {
    const auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (s.lane < 0 || s.snippetLength <= 0 ||
        s.waveforms.size() != s.events.size() * s.snippetLength)
        return fail(QStringLiteral("无效的通道事件快照"));
    QJsonObject root;
    root["schema"] = "nl_spike_lane_snapshot";
    root["schema_version"] = 1;
    root["created_at_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    root["scope"] = "retained_selected_lane_window_not_complete_recording";
    root["unit_label_semantics"] = "manual_candidate_labels_not_validated_neuron_isolation";
    root["analysis"] = metadata;
    root["lane"] = s.lane;
    root["snippet_length"] = s.snippetLength;
    root["capacity"] = s.capacity;
    root["total_detected"] = QString::number(s.totalDetected);
    root["event_count_semantics"] = "completed_full_window_events; boundary_excluded_and_pending_crossings_reported_separately";
    root["retained_events"] = s.events.size();
    root["evicted_or_not_retained_events"] = QString::number(qMax<qint64>(0, s.totalDetected - s.events.size()));
    root["observed_lane_samples"] = QString::number(s.observedSamples);
    root["threshold_volts_at_snapshot"] = s.thresholdV;
    root["noise_rms_volts_at_snapshot"] = s.noiseRmsV;
    const auto &q = s.quality;
    QJsonObject quality;
    quality["epoch"] = QString::number(q.epoch);
    quality["discontinuities"] = QString::number(q.discontinuities);
    quality["missing_source_frames"] = QString::number(q.missingSourceFrames);
    quality["subscriber_queue_dropped_frames"] = QString::number(q.queueDroppedFrames);
    quality["invalid_frames"] = QString::number(q.invalidFrames);
    quality["pending_window_events"] = QString::number(q.pendingWindowEvents);
    quality["boundary_excluded_events"] = QString::number(q.boundaryExcludedEvents);
    quality["unverified_frames"] = QString::number(q.unverifiedFrames);
    quality["stopped_early"] = q.stoppedEarly;
    quality["first_source_frame"] = QString::number(q.firstSourceFrame);
    quality["last_source_frame"] = QString::number(q.lastSourceFrame);
    quality["incomplete_coverage"] = q.incomplete();
    quality["coverage_note"] = "Covers samples actually processed; clean counters do not prove whole-session capture or biological validity.";
    root["quality"] = quality;
    QJsonArray events;
    for (int i = 0; i < s.events.size(); ++i) {
        const auto &e = s.events[i];
        if (e.lane != s.lane || e.epoch != q.epoch)
            return fail(QStringLiteral("快照的事件通道或时间线不一致"));
        QJsonObject row;
        row["epoch"] = QString::number(e.epoch);
        row["continuity_segment"] = QString::number(e.continuitySegment);
        row["sequence"] = QString::number(e.sequence);
        row["source_frame"] = QString::number(e.sourceFrame);
        row["source_sample_rate_hz"] = e.sourceSampleRate;
        row["source_time_seconds"] = e.hasSourceTime() ? QJsonValue(e.sourceTimeSeconds()) : QJsonValue();
        row["input_gain"] = e.inputGain;
        row["adc_channel"] = e.adcChannel;
        row["electrode"] = e.electrode;
        row["tdm_phase"] = e.tdmPhase;
        row["sample_stride"] = e.sampleStride;
        row["pre_samples"] = e.preSamples;
        row["candidate_unit_id"] = e.unitId;
        QJsonArray wave;
        for (int k = 0; k < s.snippetLength; ++k) {
            const float v = s.waveforms[i * s.snippetLength + k];
            if (!std::isfinite(v)) return fail(QStringLiteral("快照波形包含非有限数值"));
            wave.append(double(v));
        }
        row["waveform_input_volts"] = wave;
        events.append(row);
    }
    root["events"] = events;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) return fail(file.errorString());
    if (!file.commit()) return fail(file.errorString());
    return true;
}
} // namespace ccv2
