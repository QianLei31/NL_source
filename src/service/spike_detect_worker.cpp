#include "service/spike_detect_worker.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QElapsedTimer>
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
}

void SpikeDetectWorker::setLaneThresholdOverride(int lane, bool enabled,
                                                 double thresholdV) {
    if (lane < 0 || lane >= m_lanes || !std::isfinite(thresholdV)) return;
    {
        QMutexLocker locker(&m_thresholdOverrideLock);
        m_thresholdOverrideEnabled[lane] = enabled;
        m_thresholdOverrideV[lane] = thresholdV;
    }
    m_thresholdOverrideRevision.fetch_add(1, std::memory_order_release);
}

void SpikeDetectWorker::applyThresholdOverrides() {
    const quint64 revision =
        m_thresholdOverrideRevision.load(std::memory_order_acquire);
    if (revision == m_appliedThresholdOverrideRevision) return;

    QMutexLocker locker(&m_thresholdOverrideLock);
    for (int lane = 0; lane < m_lanes; ++lane) {
        m_proc.setChannelThresholdOverride(
            lane, m_thresholdOverrideEnabled[lane], m_thresholdOverrideV[lane]);
    }
    m_appliedThresholdOverrideRevision = revision;
}

void SpikeDetectWorker::resetLanes() {
    m_lane.clear();
    m_lane.resize(m_lanes);
}

void SpikeDetectWorker::feedLane(int lane, const QVector<double> &samples) {
    if (samples.isEmpty() || lane < 0 || lane >= m_lane.size()) return;
    LaneState &st = m_lane[lane];

    QVector<double> disp;
    QVector<bool> flags;
    m_proc.processChannel(lane, samples, disp, flags);

    const qint64 batchStart = st.total;
    st.hist.append(disp);
    for (int i = 0; i < flags.size(); ++i) {
        if (flags[i]) {
            st.pending.push_back(batchStart + i);
        }
    }
    st.total += samples.size();

    const int len = snippetLength();
    const qint64 histEnd = st.histStart + st.hist.size();  // one past the last
    int consumed = 0;
    for (int p = 0; p < st.pending.size(); ++p) {
        const qint64 t = st.pending[p];
        if (t + m_cfg.postSamples >= histEnd) {
            break;  // post-samples have not arrived yet; keep this and the rest
        }
        if (t - m_cfg.preSamples < st.histStart) {
            ++consumed;  // history already trimmed past it (should not happen)
            continue;
        }
        const qsizetype from = static_cast<qsizetype>(t - m_cfg.preSamples - st.histStart);
        for (int k = 0; k < len; ++k) {
            m_snippetScratch[k] = static_cast<float>(st.hist[from + k]);
        }
        m_store->addSnippetIfEpoch(lane, m_snippetScratch.constData(), m_lastEpoch);
        ++consumed;
    }
    if (consumed > 0) {
        st.pending.remove(0, consumed);
    }

    // Trim the history to what a future snippet could still need: either the
    // oldest waiting trigger's pre-window, or just `pre` samples of lead-in.
    const qint64 needFrom = st.pending.isEmpty()
                                ? st.total - m_cfg.preSamples
                                : st.pending.first() - m_cfg.preSamples;
    const qint64 keepFrom = qBound<qint64>(st.histStart, needFrom, st.histStart + st.hist.size());
    if (keepFrom > st.histStart) {
        st.hist.remove(0, static_cast<qsizetype>(keepFrom - st.histStart));
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
    QElapsedTimer statsTimer;
    statsTimer.start();
    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 pendingProgress = 0;

    QVector<double> frameValues(kChannelsTotal, 0.0);
    std::vector<double> medianScratch(kChannelsTotal);
    QVector<QVector<double>> laneSamples(m_lanes);

    while (!(m_stopFlag && m_stopFlag->load())) {
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
        if (epoch != m_lastEpoch || droppedBytes > 0 ||
            (info.valid() && info.firstFrame() != m_nextFrameIndex)) {
            if (epoch != m_lastEpoch) {
                m_nextFrameIndex = m_timelineFrameOrigin ? m_timelineFrameOrigin->load() : 0;
            } else if (!info.valid()) {
                m_nextFrameIndex += droppedBytes / kFrameBytes;
            }
            m_lastEpoch = epoch;
            if (info.valid()) m_nextFrameIndex = info.firstFrame();
            m_leftover.clear();
            m_proc.reset();
            resetLanes();
            m_tsRecon.reset(m_nextFrameIndex);
        }

        m_leftover.append(chunk);
        const int completeBytes = (m_leftover.size() / kFrameBytes) * kFrameBytes;
        if (completeBytes <= 0) {
            continue;
        }
        const QByteArray payload = m_leftover.left(completeBytes);
        m_leftover.remove(0, completeBytes);
        const int frameCount = payload.size() / kFrameBytes;
        if (info.valid() && info.frameIndices.size() != frameCount) continue;

        for (QVector<double> &v : laneSamples) {
            v.clear();
        }

        const int refMode = m_referenceMode ? m_referenceMode->load() : 0;
        const uchar *base = reinterpret_cast<const uchar *>(payload.constData());
        for (int f = 0; f < frameCount; ++f) {
            if (m_stopFlag && m_stopFlag->load()) return;
            const uchar *frame = base + static_cast<qsizetype>(f) * kFrameBytes;
            const quint32 ts0 =
                (qFromLittleEndian<quint32>(frame) >> kTimestampShift) & 0xFFFFFu;
            const qint64 absFrame = info.valid() ? info.frameIndices[f] : m_tsRecon.advance(ts0);
            if (absFrame != m_nextFrameIndex) {
                m_proc.reset();
                resetLanes();
                for (auto &samples : laneSamples) samples.clear();
            }
            m_nextFrameIndex = absFrame + 1;

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
                    for (int ch = 0; ch < kChannelsTotal; ++ch) {
                        laneSamples[ch * 2 + slot].push_back(frameValues[ch] - reference);
                    }
                }
            } else {
                for (int ch = 0; ch < kChannelsTotal; ++ch) {
                    laneSamples[ch].push_back(frameValues[ch] - reference);
                }
            }
        }

        if (m_timelineEpoch && epoch != m_timelineEpoch->load()) continue;
        applyThresholdOverrides();
        for (int lane = 0; lane < m_lanes; ++lane) {
            if (m_stopFlag && m_stopFlag->load()) {
                return;
            }
            if (m_timelineEpoch && epoch != m_timelineEpoch->load()) break;
            feedLane(lane, laneSamples[lane]);
        }

        pendingProgress += frameCount;
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
}

}  // namespace ccv2
