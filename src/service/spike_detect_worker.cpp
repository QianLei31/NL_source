#include "service/spike_detect_worker.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <limits>
#include <tuple>
#include <QtEndian>

#include "core/constants.h"
#include "core/channel_routing.h"

namespace ccv2 {

namespace {
constexpr double kAdcFullScaleV = 1.8;
constexpr int kStatsIntervalMs = 200;
}  // namespace

SpikeDetectWorker::SpikeDetectWorker(std::shared_ptr<ThreadSafeQueue<QByteArray>> rawQueue,
                                     SpikeSnippetStore *store,
                                     std::atomic_bool *stopFlag,
                                     const SpikeDetectConfig &cfg,
                                     const std::atomic<quint64> *timelineEpoch,
                                     const std::atomic<int> *referenceMode,
                                     qint64 initialFrameIndex,
                                     QObject *parent,
                                     const std::atomic<qint64> *timelineFrameOrigin)
    : QThread(parent),
      m_rawQueue(std::move(rawQueue)),
      m_store(store),
      m_stopFlag(stopFlag),
      m_cfg(cfg),
      m_timelineEpoch(timelineEpoch),
      m_referenceMode(referenceMode),
      m_timelineFrameOrigin(timelineFrameOrigin),
      m_lastEpoch(timelineEpoch ? timelineEpoch->load() : 0),
      m_nextFrameIndex(qMax<qint64>(0, initialFrameIndex)) {
    m_lanes = kChannelsTotal * (m_cfg.tdmEnabled ? 2 : 1);
    if (!std::isfinite(m_cfg.inputGain) || m_cfg.inputGain <= 0.0) {
        m_cfg.inputGain = 1.0;
    }
    m_cfg.preSamples = qMax(1, m_cfg.preSamples);
    m_cfg.postSamples = qMax(1, m_cfg.postSamples);
    // Detection always runs on the spike band; the snippets shown are that
    // same band, which is what a spike panel is for.
    m_cfg.proc.band = SpikeProcConfig::SpikeBand;
    m_tsRecon.reset(m_nextFrameIndex);
    m_proc.configure(m_lanes, m_cfg.proc);
    resetLanes();
    m_snippetScratch.resize(snippetLength());
    m_thresholdScratch.fill(0.0, m_lanes);
    m_rmsScratch.fill(0.0, m_lanes);
    m_thresholdOverrideEnabled.fill(false, m_lanes);
    m_thresholdOverrideV.fill(0.0, m_lanes);
    m_appliedThresholdOverrideEnabled = m_thresholdOverrideEnabled;
    m_appliedThresholdOverrideV = m_thresholdOverrideV;
    m_appliedReferenceMode = m_referenceMode ? m_referenceMode->load() : 0;
}

bool SpikeDetectWorker::setEventCallbacks(const QUuid &runId, RevisionCallback revision,
                                           EventCallback events) {
    if (runId.isNull() || isRunning() || m_started.load()) return false;
    m_runId = runId;
    m_revisionCallback = std::move(revision);
    m_eventCallback = std::move(events);
    return true;
}

bool SpikeDetectWorker::setRuleSet(SpikeRuleSet::Snapshot rules) {
    QMutexLocker locker(&m_configurationLock);
    if (rules == m_requestedRules) return true;
    // A revision names exactly one immutable definition within this run.
    if (!rules || (m_requestedRules && rules->revision() <= m_requestedRules->revision()))
        return false;
    m_requestedRules = std::move(rules);
    return true;
}

SpikeRuleSet::Snapshot SpikeDetectWorker::appliedRuleSet() const {
    QMutexLocker locker(&m_configurationLock);
    return m_appliedRules;
}

void SpikeDetectWorker::requestDrain() {
    m_drainRequested.store(true);
    if (m_rawQueue) m_rawQueue->wakeAll();
}

void SpikeDetectWorker::setLaneThresholdOverride(int lane, bool enabled, double thresholdV) {
    if (lane < 0 || lane >= m_lanes || !std::isfinite(thresholdV)) return;
    if (!enabled) thresholdV = 0.0;
    QMutexLocker locker(&m_configurationLock);
    if (m_thresholdOverrideEnabled[lane] == enabled && m_thresholdOverrideV[lane] == thresholdV) return;
    m_thresholdOverrideEnabled[lane] = enabled;
    m_thresholdOverrideV[lane] = thresholdV;
    ++m_thresholdOverrideRevision; // snapshot and revision change under ONE lock
}

SpikeRuleContext SpikeDetectWorker::contextForLaneLocked(int lane) const {
    SpikeRuleContext c;
    if (lane < 0 || lane >= m_lanes) return c;
    c.adcChannel = m_cfg.tdmEnabled ? lane / 2 : lane;
    c.tdmPhase = m_cfg.tdmEnabled ? (lane % 2 ? tdmLocalElePair(m_cfg.tdmPair02).second
                                                           : tdmLocalElePair(m_cfg.tdmPair02).first) : -1;
    c.electrode = m_cfg.tdmEnabled ? c.adcChannel * 4 + c.tdmPhase : c.adcChannel;
    c.sampleStride = m_cfg.tdmEnabled ? 4 : 1;
    c.sourceSampleRate = m_cfg.proc.sampleRate * c.sampleStride;
    c.inputGain = m_cfg.inputGain;
    c.referenceMode = m_appliedReferenceMode;
    c.filterOrder = m_cfg.proc.filterOrder;
    c.notchHz = m_cfg.proc.notchHz;
    c.highpassHz = m_cfg.proc.highpassHz;
    c.spikeLowpassHz = m_cfg.proc.spikeLowpassHz;
    c.absoluteThreshold = m_cfg.proc.absoluteThreshold;
    c.absThresholdV = m_cfg.proc.absThresholdV;
    c.rmsMultiple = m_cfg.proc.rmsMultiple;
    c.negativePolarity = m_cfg.proc.negativePolarity;
    c.refractoryMs = m_cfg.proc.refractoryMs;
    c.thresholdOverrideEnabled = m_appliedThresholdOverrideEnabled.value(lane);
    c.thresholdOverrideV = m_appliedThresholdOverrideV.value(lane);
    c.preSamples = m_cfg.preSamples;
    c.postSamples = m_cfg.postSamples;
    return c;
}

SpikeRuleContext SpikeDetectWorker::appliedRuleContext(int lane, quint64 *detectorRevision) const {
    QMutexLocker locker(&m_configurationLock);
    if (detectorRevision) *detectorRevision = m_detectorRevision.load();
    return contextForLaneLocked(lane);
}

QJsonObject SpikeDetectWorker::revisionMetadataLocked(qint64 firstSourceFrame, const char *boundarySemantics) const {
    const auto &p = m_cfg.proc;
    QJsonObject analysis;
    analysis["algorithm"] = "nl-causal-butterworth-threshold-events-v1";
    analysis["waveform_units"] = "input_referred_volts";
    analysis["waveform_band"] = "spike";
    analysis["gain_provenance"] = "manual_uniform_not_hardware_verified";
    analysis["input_gain"] = m_cfg.inputGain;
    analysis["reference_mode"] = m_appliedReferenceMode;
    analysis["lane_sample_rate_hz"] = p.sampleRate;
    analysis["source_sample_rate_hz"] = p.sampleRate * (m_cfg.tdmEnabled ? 4 : 1);
    analysis["tdm_enabled"] = m_cfg.tdmEnabled;
    analysis["tdm_pair_02"] = m_cfg.tdmPair02;
    analysis["filter_order"] = p.filterOrder;
    analysis["notch_hz"] = p.notchHz;
    analysis["highpass_hz"] = p.highpassHz;
    analysis["spike_lowpass_hz"] = p.spikeLowpassHz;
    analysis["threshold_absolute"] = p.absoluteThreshold;
    analysis["threshold_volts"] = p.absThresholdV;
    analysis["rms_multiple"] = p.rmsMultiple;
    analysis["negative_polarity"] = p.negativePolarity;
    analysis["refractory_ms"] = p.refractoryMs;
    analysis["pre_samples"] = m_cfg.preSamples;
    analysis["post_samples"] = m_cfg.postSamples;
    analysis["alignment"] = "causal_threshold_crossing_not_peak";
    QJsonArray overrides;
    for (int lane = 0; lane < m_lanes; ++lane) {
        QJsonObject o;
        const auto context = contextForLaneLocked(lane);
        o["electrode"] = context.electrode;
        o["enabled"] = context.thresholdOverrideEnabled;
        o["threshold_volts"] = context.thresholdOverrideV;
        overrides.append(o);
    }
    analysis["threshold_override_contexts"] = overrides;
    QJsonObject metadata;
    metadata["schema"] = "nl_spike_detector_revision";
    metadata["schema_version"] = 1;
    metadata["run_uuid"] = m_runId.toString(QUuid::WithoutBraces);
    metadata["detector_revision"] = QString::number(m_detectorRevision.load());
    metadata["rule_revision"] = QString::number(m_appliedRules ? m_appliedRules->revision() : 0);
    metadata["applied_epoch"] = QString::number(m_lastEpoch);
    metadata["applied_source_frame"] = QString::number(firstSourceFrame);
    metadata["applied_continuity_segment"] = QString::number(m_continuitySegment);
    metadata["boundary_semantics"] = QLatin1String(boundarySemantics);
    metadata["analysis"] = analysis;
    metadata["rule_set"] = m_appliedRules ? QJsonValue(QJsonDocument::fromJson(m_appliedRules->toJson()).object())
                                         : QJsonValue(QJsonValue::Null);
    return metadata;
}

void SpikeDetectWorker::applyConfigurationBoundary(int referenceMode, qint64 firstSourceFrame,
                                                    const char *boundarySemantics, bool forceDetectorRevision) {
    bool referenceChanged = false;
    QJsonObject metadata;
    quint64 detectorRevision, ruleRevision;
    bool announce = false;
    {
        QMutexLocker locker(&m_configurationLock);
        referenceChanged = referenceMode != m_appliedReferenceMode;
        bool thresholdsChanged = false;
        if (m_thresholdOverrideRevision != m_appliedThresholdOverrideRevision) {
            thresholdsChanged = m_thresholdOverrideEnabled != m_appliedThresholdOverrideEnabled ||
                                m_thresholdOverrideV != m_appliedThresholdOverrideV;
            m_appliedThresholdOverrideEnabled = m_thresholdOverrideEnabled;
            m_appliedThresholdOverrideV = m_thresholdOverrideV;
            m_appliedThresholdOverrideRevision = m_thresholdOverrideRevision;
            for (int lane = 0; lane < m_lanes; ++lane)
                m_proc.setChannelThresholdOverride(lane, m_appliedThresholdOverrideEnabled[lane],
                                                   m_appliedThresholdOverrideV[lane]);
        }
        m_appliedReferenceMode = referenceMode;
        if (referenceChanged || thresholdsChanged || forceDetectorRevision) ++m_detectorRevision;
        if (referenceChanged) ++m_continuitySegment;
        m_appliedRules = m_requestedRules;
        detectorRevision = m_detectorRevision.load();
        ruleRevision = m_appliedRules ? m_appliedRules->revision() : 0;
        announce = detectorRevision != m_announcedDetectorRevision || ruleRevision != m_announcedRuleRevision;
        if (announce && m_revisionCallback) metadata = revisionMetadataLocked(firstSourceFrame, boundarySemantics);
        m_announcedDetectorRevision = detectorRevision;
        m_announcedRuleRevision = ruleRevision;
    }
    if (referenceChanged) {
        // A saved context cannot describe a window mixed across references.
        m_store->markDiscontinuityIfEpoch(0, 0, 0, m_lastEpoch);
        m_proc.reset();
        resetLanes();
    }
    if (announce && m_revisionCallback) m_revisionCallback(detectorRevision, ruleRevision, metadata);
}

void SpikeDetectWorker::publishCompletedEvents() {
    if (m_completedEvents.isEmpty()) return;
    std::sort(m_completedEvents.begin(), m_completedEvents.end(), [](const auto &a, const auto &b) {
        return std::tie(a.event.sourceFrame, a.event.lane) < std::tie(b.event.sourceFrame, b.event.lane);
    });
    for (auto &record : m_completedEvents) {
        record.event.runId = m_runId;
        record.event.eventId = m_nextEventId++;
    }
    // Sort the entire bounded flush before dividing archive transport batches,
    // so IDs do not depend on detector lane order or caller chunk partition.
    for (qsizetype from = 0; from < m_completedEvents.size(); from += 256) {
        const auto batch = m_completedEvents.mid(from, 256);
        if (m_eventCallback) m_eventCallback(batch);
        for (const auto &record : batch)
            m_store->addEventIfEpoch(record.event.lane, record.waveform.constData(), record.event);
    }
    m_completedEvents.clear();
}

void SpikeDetectWorker::resetLanes(bool accountDiscarded) {
    for (int lane = 0; lane < m_lane.size(); ++lane) {
        if (accountDiscarded) m_store->addBoundaryExcludedIfEpoch(m_lane[lane].pending.size(), m_lastEpoch);
        m_store->setPendingWindowCountIfEpoch(lane, 0, m_lastEpoch);
    }
    m_lane.clear();
    m_lane.resize(m_lanes);
}

void SpikeDetectWorker::feedLane(int lane, const QVector<double> &samples, const QVector<qint64> &frames) {
    if (samples.isEmpty() || samples.size() != frames.size() || lane < 0 || lane >= m_lane.size()) return;
    LaneState &st = m_lane[lane];

    QVector<double> disp;
    QVector<bool> flags;
    m_proc.processChannel(lane, samples, disp, flags);

    const qint64 batchStart = st.total;
    st.hist.append(disp);
    st.frames.append(frames);
    for (int i = 0; i < flags.size(); ++i) {
        if (flags[i]) {
            PendingCrossing crossing;
            crossing.sample = batchStart + i;
            {
                QMutexLocker locker(&m_configurationLock);
                crossing.context = contextForLaneLocked(lane);
                crossing.rules = m_appliedRules;
                crossing.detectorRevision = m_detectorRevision.load();
            }
            st.pending.push_back(std::move(crossing));
        }
    }
    st.total += samples.size();
    m_store->addCoverageIfEpoch(lane, samples.size(), frames.first(), frames.last(), m_lastEpoch);

    const int len = snippetLength();
    const qint64 histEnd = st.histStart + st.hist.size();  // one past the last
    int consumed = 0;
    for (int p = 0; p < st.pending.size(); ++p) {
        const auto &crossing = st.pending[p];
        const qint64 t = crossing.sample;
        if (t + m_cfg.postSamples >= histEnd) {
            break;  // post-samples have not arrived yet; keep this and the rest
        }
        if (t - m_cfg.preSamples < st.histStart) {
            m_store->addBoundaryExcludedIfEpoch(1, m_lastEpoch);
            ++consumed;  // pre-window crosses the start of this valid segment
            continue;
        }
        const qsizetype from = static_cast<qsizetype>(t - m_cfg.preSamples - st.histStart);
        for (int k = 0; k < len; ++k) {
            m_snippetScratch[k] = static_cast<float>(st.hist[from + k]);
        }
        SpikeEvent event;
        event.epoch = m_lastEpoch;
        event.continuitySegment = m_continuitySegment;
        event.sourceFrame = st.frames[static_cast<qsizetype>(t - st.histStart)];
        event.sampleStride = m_cfg.tdmEnabled ? kTdmPhaseCount : 1;
        event.sourceSampleRate = m_cfg.proc.sampleRate * event.sampleStride;
        event.inputGain = m_cfg.inputGain;
        event.adcChannel = m_cfg.tdmEnabled ? lane / 2 : lane;
        event.tdmPhase = m_cfg.tdmEnabled ? tdmPhaseForFrame(event.sourceFrame) : -1;
        event.electrode = m_cfg.tdmEnabled ? event.adcChannel * kTdmPhaseCount + event.tdmPhase : lane;
        event.preSamples = m_cfg.preSamples;
        event.lane = lane;
        event.detectorRevision = crossing.detectorRevision;
        if (crossing.rules) {
            const auto result = crossing.rules->classify(event, m_snippetScratch.constData(), len, crossing.context);
            event.ruleRevision = result.ruleRevision;
            event.unitId = result.unitId;
            event.classification = result.status;
        }
        m_completedEvents.append({event, m_snippetScratch});
        // The raw-frame flush budget bounds all lanes' completed snippets;
        // callbacks are sliced only after their chronological merge.
        ++consumed;
    }
    if (consumed > 0) {
        st.pending.remove(0, consumed);
    }

    m_store->setPendingWindowCountIfEpoch(lane, st.pending.size(), m_lastEpoch);

    // Trim the history to what a future snippet could still need: either the
    // oldest waiting trigger's pre-window, or just `pre` samples of lead-in.
    const qint64 needFrom = st.pending.isEmpty()
                                ? st.total - m_cfg.preSamples
                                : st.pending.first().sample - m_cfg.preSamples;
    const qint64 keepFrom = qBound<qint64>(st.histStart, needFrom, st.histStart + st.hist.size());
    if (keepFrom > st.histStart) {
        st.hist.remove(0, static_cast<qsizetype>(keepFrom - st.histStart));
        st.frames.remove(0, static_cast<qsizetype>(keepFrom - st.histStart));
        st.histStart = keepFrom;
    }
}

void SpikeDetectWorker::publishStats() {
    for (int lane = 0; lane < m_lanes; ++lane) {
        m_thresholdScratch[lane] =
            m_proc.detectorReady(lane) ? m_proc.currentThreshold(lane) : 0.0;
        m_rmsScratch[lane] = m_proc.currentRms(lane);
    }
    m_store->setStatsIfEpoch(m_thresholdScratch, m_rmsScratch, m_lastEpoch);
}

void SpikeDetectWorker::run() {
    m_started.store(true);
    m_drained.store(false);
    applyConfigurationBoundary(m_referenceMode ? m_referenceMode->load() : 0,
                               m_nextFrameIndex, "subscription_origin");
    QElapsedTimer statsTimer;
    statsTimer.start();
    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 pendingProgress = 0;

    QVector<double> frameValues(kChannelsTotal, 0.0);
    std::vector<double> medianScratch(kChannelsTotal);
    QVector<QVector<double>> laneSamples(m_lanes);
    QVector<QVector<qint64>> laneFrames(m_lanes);
    int blockReferenceMode = m_referenceMode ? m_referenceMode->load() : 0;
    qint64 firstBufferedSourceFrame = -1;
    bool epochResetPending = false;
    // One lane can complete at most one event per new waveform sample. This
    // conservative raw-frame bound therefore caps the merge buffer at ~8 MiB,
    // even with maximal windows and unrealistically dense detections.
    const qint64 recordBytes = qint64(snippetLength()) * sizeof(float) + sizeof(SpikeArchiveRecord);
    const int framesPerFlush = int(std::clamp<qint64>((8LL * 1024 * 1024) / (m_lanes * recordBytes), 1, 256));
    const auto flushLanes = [&]() {
        if (m_stopFlag && m_stopFlag->load()) return;
        if (m_timelineEpoch && m_lastEpoch != m_timelineEpoch->load()) return;
        // Empty/end-of-block flushes must not invent an application coordinate
        // for a GUI request made while processing is paused or input is absent.
        if (firstBufferedSourceFrame < 0) return;
        applyConfigurationBoundary(blockReferenceMode, firstBufferedSourceFrame,
                                   epochResetPending ? "epoch_reset_at_first_processed_input_frame"
                                                     : "first_input_frame_of_processing_flush",
                                   epochResetPending);
        epochResetPending = false;
        for (int lane = 0; lane < m_lanes; ++lane) {
            if (m_stopFlag && m_stopFlag->load()) break;
            if (m_timelineEpoch && m_lastEpoch != m_timelineEpoch->load()) break;
            feedLane(lane, laneSamples[lane], laneFrames[lane]);
            laneSamples[lane].clear();
            laneFrames[lane].clear();
        }
        publishCompletedEvents();
        firstBufferedSourceFrame = -1;
    };

    while (!(m_stopFlag && m_stopFlag->load())) {
        if (m_drainRequested.load() && m_rawQueue->size() == 0) break;
        const quint64 waitingEpoch = m_timelineEpoch ? m_timelineEpoch->load() : 0;
        QByteArray chunk;
        StreamBlockInfo info;
        qint64 droppedBytes = 0;
        if (!m_rawQueue->pop(chunk, 200, &droppedBytes, &info)) {
            continue;
        }
        if (m_stopFlag && m_stopFlag->load()) {
            break;
        }

        const quint64 epoch = info.valid() ? info.epoch : waitingEpoch;
        if (m_timelineEpoch && epoch != m_timelineEpoch->load()) continue;
        const int refMode = m_referenceMode ? m_referenceMode->load() : 0;
        blockReferenceMode = refMode;
        const bool timelineChanged = epoch != m_lastEpoch;
        if (timelineChanged || droppedBytes > 0 ||
            (info.valid() && info.firstFrame() != m_nextFrameIndex)) {
            if (epoch != m_lastEpoch) {
                m_nextFrameIndex = m_timelineFrameOrigin ? m_timelineFrameOrigin->load() : 0;
                m_continuitySegment = 0;
            } else {
                const qint64 missing = info.valid() ? qMax<qint64>(0, info.firstFrame() - m_nextFrameIndex)
                                                     : droppedBytes / kFrameBytes;
                m_store->markDiscontinuityIfEpoch(missing, droppedBytes / kFrameBytes, 0, epoch);
                ++m_continuitySegment;
                if (!info.valid()) m_nextFrameIndex += droppedBytes / kFrameBytes;
            }
            m_lastEpoch = epoch;
            if (info.valid()) m_nextFrameIndex = info.firstFrame();
            m_leftover.clear();
            m_proc.reset();
            resetLanes(!timelineChanged);
            m_tsRecon.reset(m_nextFrameIndex);
            firstBufferedSourceFrame = -1;
            if (timelineChanged) epochResetPending = true;
        }

        m_leftover.append(chunk);
        const int completeBytes = (m_leftover.size() / kFrameBytes) * kFrameBytes;
        if (completeBytes <= 0) {
            continue;
        }
        const QByteArray payload = m_leftover.left(completeBytes);
        m_leftover.remove(0, completeBytes);
        const int frameCount = payload.size() / kFrameBytes;
        if (info.valid() && info.frameIndices.size() != frameCount) {
            m_store->markDiscontinuityIfEpoch(0, 0, frameCount, epoch);
            ++m_continuitySegment;
            m_proc.reset();
            resetLanes();
            m_nextFrameIndex = info.nextFrame();
            pendingProgress += frameCount;
            continue;
        }
        if (info.integrityUnknown || (!info.frameValid.isEmpty() && info.frameValid.size() != frameCount)) {
            // Legacy damaged recordings without exact invalid intervals cannot
            // be safely interpreted as neural signal. Keep raw replay usable,
            // but explicitly exclude this block from neural exposure/events.
            m_store->markDiscontinuityIfEpoch(0, 0, frameCount, epoch);
            ++m_continuitySegment;
            m_proc.reset();
            resetLanes();
            if (info.valid()) m_nextFrameIndex = info.nextFrame();
            pendingProgress += frameCount;
            continue;
        }

        if (info.frameValid.isEmpty()) m_store->markUnverifiedFramesIfEpoch(frameCount, epoch);
        for (QVector<double> &v : laneSamples) v.clear();
        for (QVector<qint64> &v : laneFrames) v.clear();

        firstBufferedSourceFrame = -1;
        const uchar *base = reinterpret_cast<const uchar *>(payload.constData());
        int processedFrames = 0;
        for (int f = 0; f < frameCount; ++f) {
            if (m_stopFlag && m_stopFlag->load()) break;
            ++processedFrames;
            const uchar *frame = base + static_cast<qsizetype>(f) * kFrameBytes;
            const quint32 ts0 =
                (qFromLittleEndian<quint32>(frame) >> kTimestampShift) & 0xFFFFFu;
            bool frameValid = info.frameValid.isEmpty() || info.frameValid[f];
            for (int ch = 1; frameValid && ch < kChannelsTotal; ++ch) {
                const quint32 timestamp = qFromLittleEndian<quint32>(frame + ch * kBytesPerPoint) >> kTimestampShift;
                frameValid = timestamp == ts0;
            }
            qint64 absFrame;
            if (info.valid()) absFrame = info.frameIndices[f];
            else if (frameValid) absFrame = m_tsRecon.advance(ts0);
            else { absFrame = m_tsRecon.nextIndex(); m_tsRecon.skipFrames(1); }
            if (absFrame != m_nextFrameIndex) {
                // Preserve complete pre-gap events, but never join a waveform
                // or detector history across missing source frames.
                flushLanes();
                m_store->markDiscontinuityIfEpoch(qMax<qint64>(0, absFrame - m_nextFrameIndex), 0, 0, epoch);
                ++m_continuitySegment;
                m_proc.reset();
                resetLanes();
                for (auto &samples : laneSamples) samples.clear();
                for (auto &frames : laneFrames) frames.clear();
            }
            m_nextFrameIndex = absFrame + 1;
            if (!frameValid) {
                flushLanes();
                m_store->markDiscontinuityIfEpoch(0, 0, 1, epoch);
                ++m_continuitySegment;
                m_proc.reset();
                resetLanes();
                continue;
            }

            for (int ch = 0; ch < kChannelsTotal; ++ch) {
                const quint32 raw =
                    qFromLittleEndian<quint32>(frame + ch * kBytesPerPoint);
                frameValues[ch] =
                    (static_cast<double>(raw & kAdcSampleMask) / 4096.0 *
                     kAdcFullScaleV) /
                    m_cfg.inputGain;
            }

            double reference = 0.0;
            if (refMode == 1) {
                double sum = 0.0;
                for (double v : frameValues) sum += v;
                reference = sum / static_cast<double>(kChannelsTotal);
            } else if (refMode == 2) {
                std::copy(frameValues.cbegin(), frameValues.cend(), medianScratch.begin());
                const auto mid = medianScratch.begin() + kChannelsTotal / 2;
                std::nth_element(medianScratch.begin(), mid, medianScratch.end());
                reference = *mid;
            }

            if (m_cfg.tdmEnabled) {
                // Demultiplex the true four-phase stream and retain the pair
                // selected by the global 0/2 versus 1/3 display mode.
                const int slot = tdmVisibleSlotForPhase(
                    tdmPhaseForFrame(absFrame), m_cfg.tdmPair02);
                if (slot >= 0) {
                    if (firstBufferedSourceFrame < 0) firstBufferedSourceFrame = absFrame;
                    for (int ch = 0; ch < kChannelsTotal; ++ch) {
                        laneSamples[ch * 2 + slot].push_back(frameValues[ch] - reference);
                        laneFrames[ch * 2 + slot].push_back(absFrame);
                    }
                }
            } else {
                if (firstBufferedSourceFrame < 0) firstBufferedSourceFrame = absFrame;
                for (int ch = 0; ch < kChannelsTotal; ++ch) {
                    laneSamples[ch].push_back(frameValues[ch] - reference);
                    laneFrames[ch].push_back(absFrame);
                }
            }
            if ((f + 1) % framesPerFlush == 0) flushLanes();
        }

        if (m_timelineEpoch && epoch != m_timelineEpoch->load()) continue;
        flushLanes();

        pendingProgress += processedFrames;
        if (progressTimer.elapsed() >= 200) {
            emit framesProcessed(pendingProgress);
            pendingProgress = 0;
            progressTimer.restart();
        }
        if (statsTimer.elapsed() >= kStatsIntervalMs) {
            publishStats();
            statsTimer.restart();
        }
    }
    publishCompletedEvents();
    const bool drained = m_drainRequested.load() && !(m_stopFlag && m_stopFlag->load()) && m_rawQueue->size() == 0;
    if (drained) {
        resetLanes(); // explicitly account for windows missing post-samples at EOF
        if (!m_leftover.isEmpty()) {
            m_store->markDiscontinuityIfEpoch(0, 0, 1, m_lastEpoch);
            m_store->markAnalysisIncomplete(m_lastEpoch);
            m_leftover.clear();
        }
    }
    publishStats();
    if (pendingProgress) emit framesProcessed(pendingProgress);
    m_drained.store(drained);
}

}  // namespace ccv2
