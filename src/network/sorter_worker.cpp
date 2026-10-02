#include "network/sorter_worker.h"

#include <QElapsedTimer>
#include <QMap>
#include <QtEndian>

#include <limits>

#include "core/constants.h"
#include "data/data_store.h"
#include "io/session_recorder.h"

namespace ccv2 {

SorterWorker::SorterWorker(std::shared_ptr<ThreadSafeQueue<QByteArray>> inputQueue,
                           DataStore *dataStore,
                           const QVector<int> &selectedChannels,
                           SessionRecorder *recorder,
                           std::function<bool()> saveEnabledFn,
                           std::function<bool()> processEnabledFn,
                           std::atomic_bool *stopFlag,
                           const std::atomic<quint64> *timelineEpoch,
                           QObject *parent)
    : QThread(parent),
      m_inputQueue(std::move(inputQueue)),
      m_dataStore(dataStore),
      m_selectedChannels(selectedChannels),
      m_recorder(recorder),
      m_saveEnabledFn(std::move(saveEnabledFn)),
      m_processEnabledFn(std::move(processEnabledFn)),
      m_stopFlag(stopFlag),
      m_timelineEpoch(timelineEpoch),
      m_lastEpoch(timelineEpoch ? timelineEpoch->load() : 0) {}

void SorterWorker::run() {
    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 pendingProgressFrames = 0;
    int progressLeftoverBytes = 0;

    while (!m_stopFlag->load()) {
        const quint64 waitingEpoch = m_timelineEpoch ? m_timelineEpoch->load() : 0;
        QByteArray chunk;
        qint64 droppedBytesBefore = 0;
        StreamBlockInfo info;
        if (!m_inputQueue->pop(chunk, 500, &droppedBytesBefore, &info)) {
            continue;
        }
        if (m_stopFlag->load()) {
            break;
        }

        if (!m_processEnabledFn()) {
            continue;
        }

        // Timeline barrier (see DataSorter): leftover bytes from before a seek
        // belong to the previous timeline segment.
        const quint64 chunkEpoch = info.valid() ? info.epoch : waitingEpoch;
        if (m_timelineEpoch && chunkEpoch != m_timelineEpoch->load()) continue;
        if (chunkEpoch != m_lastEpoch) {
            m_incomplete.clear();
            m_lastEpoch = chunkEpoch;
            m_tsRecon.reset(0);
        } else if (droppedBytesBefore > 0 && !info.valid()) {
            // Chunks evicted from our queue sat strictly before this one.
            // Advance every channel's global index over them so absolute
            // sample numbering — and the TDM slot parity derived from it —
            // stays exact even across an odd frame drop. The partial leftover
            // lost its continuation, so discard it too.
            m_incomplete.clear();
            m_dataStore->skipFramesIfEpoch(droppedBytesBefore / kFrameBytes,
                                           chunkEpoch);
            // Queue accounting already covered this gap; re-seed so the
            // timestamp check does not count it a second time.
            m_tsRecon.reset(0);
        }
        if (info.valid()) {
            m_incomplete.clear();
            if (!m_dataStore->alignFramesIfEpoch(info.firstFrame(), chunkEpoch)) continue;
        }

        if (m_recorder && m_saveEnabledFn() && !m_recordingFailed &&
            !m_recorder->write(chunk)) {
            m_recordingFailed = true;
            emit recordingError(QStringLiteral("ADC_DATA.bin 写入失败，已停止本次录制"));
        }

        const QByteArray fullData = m_incomplete + chunk;
        const int usable = fullData.size() - (fullData.size() % kFrameBytes);

        if (usable <= 0) {
            m_incomplete = fullData;
            continue;
        }

        const QByteArray valid = fullData.left(usable);
        m_incomplete = fullData.mid(usable);

        const int frameCount = valid.size() / kFrameBytes;
        if (info.valid() && info.frameIndices.size() != frameCount) continue;
        pendingProgressFrames += frameCount;
        progressLeftoverBytes = m_incomplete.size();
        if (progressTimer.elapsed() >= 100) {
            emit framesParsed(static_cast<int>(qMin<qint64>(pendingProgressFrames,
                                                            std::numeric_limits<int>::max())),
                              progressLeftoverBytes);
            pendingProgressFrames = 0;
            progressTimer.restart();
        }

        // A seek landed while this chunk was in flight — drop it.
        if (m_timelineEpoch && m_timelineEpoch->load() != chunkEpoch) {
            m_incomplete.clear();
            continue;
        }

        // Track the hardware timestamp (per-frame, from channel 0) to detect an
        // upstream frame loss inside this chunk. The store labels samples by a
        // contiguous index, so a mid-chunk loss is handled like a mini-seek:
        // advance the index over the whole chunk plus the lost frames and drop
        // this chunk from the page-3 buffers, so the next chunk resumes with the
        // correct TDM electrode parity (a brief reset of the trace/FFT).
        qint64 chunkGap = 0;
        for (int f = 0; f < frameCount; ++f) {
            const char *frame = valid.constData() + f * kFrameBytes;
            const quint32 ts0 = (qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar *>(frame)) >> kTimestampShift) & 0xFFFFFu;
            qint64 gap = 0;
            if (info.valid()) {
                if (f > 0 && info.frameIndices[f] != info.frameIndices[f - 1] + 1) gap = 1;
            } else {
                m_tsRecon.advance(ts0, &gap);
            }
            chunkGap += gap;
        }

        if (chunkGap > 0) {
            if (info.valid()) {
                m_dataStore->alignFramesIfEpoch(info.nextFrame(), chunkEpoch, true);
            } else {
                m_dataStore->skipFramesIfEpoch(static_cast<qint64>(frameCount) + chunkGap, chunkEpoch);
            }
            continue;
        }

        for (int ch : m_selectedChannels) {
            if (m_stopFlag->load()) {
                return;
            }
            QVector<qint32> values;
            values.reserve(frameCount);
            for (int f = 0; f < frameCount; ++f) {
                if ((f & 0xFF) == 0 && m_stopFlag->load()) {
                    return;
                }
                const char *frame = valid.constData() + f * kFrameBytes;
                const int offset = ch * kBytesPerPoint;
                const quint32 raw = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(frame + offset));
                // low 12 bits = ADC sample; high 20 bits = frame timestamp -> strip it
                const qint32 v = static_cast<qint32>(raw & kAdcSampleMask);
                values.push_back(v);
            }
            m_dataStore->appendChannelValuesIfEpoch(ch, values, chunkEpoch);
        }
    }

    if (pendingProgressFrames > 0) {
        emit framesParsed(static_cast<int>(qMin<qint64>(pendingProgressFrames,
                                                        std::numeric_limits<int>::max())),
                          progressLeftoverBytes);
    }
}

}  // namespace ccv2
