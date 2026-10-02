#include "network/data_sorter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <QElapsedTimer>
#include <QtEndian>

#include "core/constants.h"
#include "core/channel_routing.h"

namespace ccv2 {

namespace {
constexpr double kAdcFullScaleV = 1.8;
}

DataSorter::DataSorter(std::shared_ptr<ThreadSafeQueue<QByteArray>> rawQueue,
                       std::shared_ptr<RealtimeStreamState> state,
                       std::atomic_bool *stopFlag,
                       const std::atomic<quint64> *timelineEpoch,
                       const std::atomic<int> *referenceMode,
                       const std::atomic<qint64> *timelineFrameOrigin,
                       qint64 initialFrameIndex,
                       QObject *parent)
    : QThread(parent),
      m_rawQueue(std::move(rawQueue)),
      m_state(std::move(state)),
      m_stopFlag(stopFlag),
      m_timelineEpoch(timelineEpoch),
      m_referenceMode(referenceMode),
      m_timelineFrameOrigin(timelineFrameOrigin),
      m_lastEpoch(timelineEpoch ? timelineEpoch->load() : 0),
      m_nextFrameIndex(qMax<qint64>(0, initialFrameIndex)),
      m_lastReferenceMode(referenceMode ? referenceMode->load() : 0) {
    m_tsRecon.reset(m_nextFrameIndex);
    resetMetricWindow();
}

void DataSorter::resetMetricWindow() {
    const double inf = std::numeric_limits<double>::infinity();
    m_metricFrameCount = 0;
    m_metricPacketLoss = false;
    m_metricSum.fill(0.0, kChannelsTotal);
    m_metricSumSq.fill(0.0, kChannelsTotal);
    m_metricMin.fill(inf, kChannelsTotal);
    m_metricMax.fill(-inf, kChannelsTotal);
    m_metricOrigSum.fill(0.0, kChannelsTotal);
    m_metricOrigMin.fill(inf, kChannelsTotal);
    m_metricOrigMax.fill(-inf, kChannelsTotal);

    const int tdmSize = kChannelsTotal * kTdmPhaseCount;
    m_tdmSum.fill(0.0, tdmSize);
    m_tdmSumSq.fill(0.0, tdmSize);
    m_tdmMin.fill(inf, tdmSize);
    m_tdmMax.fill(-inf, tdmSize);
    m_tdmOrigSum.fill(0.0, tdmSize);
    m_tdmOrigMin.fill(inf, tdmSize);
    m_tdmOrigMax.fill(-inf, tdmSize);
    m_tdmCount.fill(0, tdmSize);
}

void DataSorter::run() {
    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 pendingProgressFrames = 0;
    int progressLeftoverBytes = 0;

    while (!m_stopFlag->load()) {
        const quint64 waitingEpoch = m_timelineEpoch ? m_timelineEpoch->load() : 0;
        QByteArray chunk;
        qint64 droppedBytesBefore = 0;
        StreamBlockInfo info;
        if (!m_rawQueue->pop(chunk, 200, &droppedBytesBefore, &info)) {
            continue;
        }
        if (m_stopFlag->load()) {
            break;
        }

        const quint64 chunkEpoch = info.valid() ? info.epoch : waitingEpoch;
        if (m_timelineEpoch && chunkEpoch != m_timelineEpoch->load()) continue;
        if (chunkEpoch != m_lastEpoch) {
            m_leftover.clear();
            m_lastEpoch = chunkEpoch;
            m_nextFrameIndex = info.valid() ? info.firstFrame() : m_timelineFrameOrigin
                                   ? qMax<qint64>(0, m_timelineFrameOrigin->load())
                                   : 0;
            m_tsRecon.reset(m_nextFrameIndex);
            resetMetricWindow();
        } else if (droppedBytesBefore > 0 && !info.valid()) {
            // Chunks evicted from our queue sat strictly before this one.
            // Advancing the frame counter over them keeps the absolute
            // numbering — and the TDM slot parity derived from it — exact
            // even when an odd number of frames was dropped. Any leftover
            // partial frame lost its continuation with the dropped chunk
            // (chunks are frame-aligned in practice, so this is defensive).
            m_nextFrameIndex += droppedBytesBefore / kFrameBytes;
            m_leftover.clear();
            // The dropped chunk broke timestamp continuity; re-seed so its gap
            // is not double-counted (queue accounting already handled it).
            m_tsRecon.reset(m_nextFrameIndex);
        }

        const bool gapAtStart = info.valid() && info.firstFrame() != m_nextFrameIndex;
        if (info.valid()) {
            m_leftover.clear();
            m_nextFrameIndex = info.firstFrame();
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
        const qint64 chunkStartFrame = m_nextFrameIndex;
        pendingProgressFrames += frameCount;
        progressLeftoverBytes = m_leftover.size();
        // UI status only needs human-scale updates. Emitting once per 64-frame
        // network batch floods the GUI event queue at high sample rates and
        // makes page activation appear frozen.
        if (progressTimer.elapsed() >= 100) {
            emit framesParsed(static_cast<int>(qMin<qint64>(pendingProgressFrames,
                                                            std::numeric_limits<int>::max())),
                              progressLeftoverBytes);
            pendingProgressFrames = 0;
            progressTimer.restart();
        }

        QVector<int> channels;
        int maxSamples = 1200;
        int metricWindowFrames = 2000;
        {
            QMutexLocker locker(&m_state->lock);
            channels = m_state->channels;
            maxSamples = qMax(1, m_state->maxSamples);
            metricWindowFrames = qMax(1, m_state->metricWindowFrames);
            if (m_state->droppedFrames != m_lastDroppedFrames) {
                m_metricPacketLoss = true;
                m_lastDroppedFrames = m_state->droppedFrames;
            }
        }

        const int refMode = m_referenceMode ? m_referenceMode->load() : 0;
        if (refMode != m_lastReferenceMode) {
            m_lastReferenceMode = refMode;
            resetMetricWindow();
        }

        QVector<bool> selected(kChannelsTotal, false);
        QMap<int, QVector<double>> valuesByChannel;
        for (int ch : channels) {
            if (ch < 0 || ch >= kChannelsTotal) {
                continue;
            }
            selected[ch] = true;
            QVector<double> values;
            values.reserve(frameCount);
            valuesByChannel.insert(ch, std::move(values));
        }

        QVector<double> frameValues(kChannelsTotal, 0.0);
        std::vector<double> medianScratch;
        if (refMode == 2) {
            medianScratch.resize(kChannelsTotal);
        }

        qint64 chunkGap = gapAtStart ? 1 : 0;
        for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            // Page workers are display consumers, not recorders. Once their
            // page is hidden, stale queued frames have no value and must not
            // hold the GUI in a multi-second wait during tab switching.
            if (m_stopFlag->load()) {
                return;
            }
            const char *frame = payload.constData() + frameIndex * kFrameBytes;
            // The TDM phase is the true frame number modulo four. Recover that
            // from the per-frame timestamp (high 20 bits, shared by all points)
            // so an upstream drop advances the count instead of flipping the
            // electrodes. A zero/absent timestamp leaves the reconciler dormant,
            // giving exactly (chunkStartFrame + frameIndex) as before.
            const quint32 ts0 = (qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar *>(frame)) >> kTimestampShift) & 0xFFFFFu;
            qint64 gap = 0;
            const qint64 absFrame = info.valid() ? info.frameIndices[frameIndex]
                                                 : m_tsRecon.advance(ts0, &gap);
            if (info.valid() && frameIndex > 0 &&
                absFrame != info.frameIndices[frameIndex - 1] + 1) gap = 1;
            chunkGap += gap;
            if (chunkGap > 0) m_metricPacketLoss = true;
            const int tdmPhase = tdmPhaseForFrame(absFrame);

            for (int ch = 0; ch < kChannelsTotal; ++ch) {
                const quint32 raw = qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar *>(frame + ch * kBytesPerPoint));
                const quint32 adc = raw & kAdcSampleMask;
                frameValues[ch] = static_cast<double>(adc) / 4096.0 * kAdcFullScaleV;
            }

            double reference = 0.0;
            if (refMode == 1) {
                double sum = 0.0;
                for (double value : frameValues) {
                    sum += value;
                }
                reference = sum / static_cast<double>(kChannelsTotal);
            } else if (refMode == 2) {
                std::copy(frameValues.cbegin(), frameValues.cend(), medianScratch.begin());
                const auto middle = medianScratch.begin() + kChannelsTotal / 2;
                std::nth_element(medianScratch.begin(), middle, medianScratch.end());
                reference = *middle;
            }

            for (int ch = 0; ch < kChannelsTotal; ++ch) {
                const double original = frameValues[ch];
                const double value = original - reference;
                m_metricSum[ch] += value;
                m_metricSumSq[ch] += value * value;
                m_metricMin[ch] = qMin(m_metricMin[ch], value);
                m_metricMax[ch] = qMax(m_metricMax[ch], value);
                m_metricOrigSum[ch] += original;
                m_metricOrigMin[ch] = qMin(m_metricOrigMin[ch], original);
                m_metricOrigMax[ch] = qMax(m_metricOrigMax[ch], original);

                const int tdmIndex = ch * kTdmPhaseCount + tdmPhase;
                m_tdmSum[tdmIndex] += value;
                m_tdmSumSq[tdmIndex] += value * value;
                m_tdmMin[tdmIndex] = qMin(m_tdmMin[tdmIndex], value);
                m_tdmMax[tdmIndex] = qMax(m_tdmMax[tdmIndex], value);
                m_tdmOrigSum[tdmIndex] += original;
                m_tdmOrigMin[tdmIndex] = qMin(m_tdmOrigMin[tdmIndex], original);
                m_tdmOrigMax[tdmIndex] = qMax(m_tdmOrigMax[tdmIndex], original);
                ++m_tdmCount[tdmIndex];

                if (selected[ch]) {
                    valuesByChannel[ch].push_back(value);
                }
            }
            ++m_metricFrameCount;
        }

        const bool publishMetrics = m_metricFrameCount >= metricWindowFrames;
        QVector<RealtimeChannelMetric> metrics;
        QVector<RealtimeChannelMetric> tdmMetrics;
        if (publishMetrics) {
            metrics.resize(kChannelsTotal);
            const double invCount = 1.0 / static_cast<double>(m_metricFrameCount);
            for (int ch = 0; ch < kChannelsTotal; ++ch) {
                const double mean = m_metricSum[ch] * invCount;
                const double meanSq = m_metricSumSq[ch] * invCount;
                const double variance = qMax(0.0, meanSq - mean * mean);
                auto &metric = metrics[ch];
                // mean is the raw DC level (heatmap maps it onto 0..1.8 V);
                // rms/p2p stay on the referenced signal so the software
                // reference keeps doing its job for the AC quantities.
                metric.mean = m_metricOrigSum[ch] * invCount;
                metric.rms = std::sqrt(variance);
                metric.p2p = m_metricMax[ch] - m_metricMin[ch];
                metric.saturated = m_metricOrigMin[ch] <= 0.01 ||
                                   m_metricOrigMax[ch] >= 1.79;
                metric.packetLoss = m_metricPacketLoss;
                metric.valid = true;
            }

            tdmMetrics.resize(kChannelsTotal * kTdmPhaseCount);
            for (int index = 0; index < tdmMetrics.size(); ++index) {
                if (m_tdmCount[index] <= 0) {
                    continue;
                }
                const double invCountForSlot =
                    1.0 / static_cast<double>(m_tdmCount[index]);
                const double mean = m_tdmSum[index] * invCountForSlot;
                const double meanSq = m_tdmSumSq[index] * invCountForSlot;
                const double variance = qMax(0.0, meanSq - mean * mean);
                auto &metric = tdmMetrics[index];
                metric.mean = m_tdmOrigSum[index] * invCountForSlot;
                metric.rms = std::sqrt(variance);
                metric.p2p = m_tdmMax[index] - m_tdmMin[index];
                metric.saturated = m_tdmOrigMin[index] <= 0.01 ||
                                   m_tdmOrigMax[index] >= 1.79;
                metric.packetLoss = m_metricPacketLoss;
                metric.valid = true;
            }
        }

        if (m_stopFlag->load()) {
            break;
        }
        {
            QMutexLocker locker(&m_state->lock);
            if ((m_timelineEpoch && m_timelineEpoch->load() != chunkEpoch) ||
                m_state->timelineEpoch != chunkEpoch) {
                m_leftover.clear();
                m_nextFrameIndex = m_timelineFrameOrigin
                                       ? qMax<qint64>(0, m_timelineFrameOrigin->load())
                                       : 0;
                m_tsRecon.reset(m_nextFrameIndex);
                resetMetricWindow();
                continue;
            }

            if (publishMetrics) {
                m_state->metrics = metrics;
                m_state->tdmMetrics = tdmMetrics;
                ++m_state->metricsEpoch;
            }
            if (chunkGap > 0) {
                // Upstream loss inside this chunk breaks the contiguous-index
                // assumption the waveform/sweep buffers rely on. The heatmap
                // metrics above were already binned per-frame with the correct
                // (timestamp-recovered) slot; the trace buffers reset here and
                // resume next chunk at the corrected index, so their electrode
                // parity is right too — at the cost of a few ms of trace.
                const qint64 resumeIndex = info.valid() ? info.nextFrame() : m_tsRecon.nextIndex();
                for (int ch : channels) {
                    m_state->buffers[ch].clear();
                    m_state->totalSamples[ch] = resumeIndex;
                }
            } else {
                for (auto it = valuesByChannel.cbegin(); it != valuesByChannel.cend(); ++it) {
                    auto &queue = m_state->buffers[it.key()];
                    for (double value : it.value()) {
                        queue.enqueue(value);
                    }
                    m_state->totalSamples[it.key()] = chunkStartFrame + it.value().size();
                    while (queue.size() > maxSamples) {
                        queue.dequeue();
                    }
                }
            }
        }

        m_nextFrameIndex = info.valid() ? info.nextFrame() : m_tsRecon.nextIndex();
        if (publishMetrics) {
            resetMetricWindow();
        }
    }

    if (pendingProgressFrames > 0) {
        emit framesParsed(static_cast<int>(qMin<qint64>(pendingProgressFrames,
                                                        std::numeric_limits<int>::max())),
                          progressLeftoverBytes);
    }
}

}  // namespace ccv2
