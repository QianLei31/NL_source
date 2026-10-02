#include "network/timestamp_checker.h"

#include <QElapsedTimer>
#include <QtEndian>

#include <limits>

#include "core/constants.h"

namespace ccv2 {

namespace {

constexpr quint32 kTimestampModulo = 1u << 20;
constexpr quint32 kTimestampMask = kTimestampModulo - 1u;
constexpr int kAutoCalibrationTransitions = 8;
constexpr int kProgressIntervalMs = 250;
constexpr int kFirstCompleteFrameTimeoutMs = 15000;

quint32 frameTimestamp(const char *point)
{
    const quint32 raw = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(point));
    return (raw >> kTimestampShift) & kTimestampMask;
}

}  // namespace

TimestampContinuityAnalyzer::TimestampContinuityAnalyzer(quint32 expectedStep)
{
    reset(expectedStep);
}

void TimestampContinuityAnalyzer::reset(quint32 expectedStep)
{
    m_leftover.clear();
    m_stats = {};
    m_configuredStep = expectedStep & kTimestampMask;
    m_stats.expectedStep = m_configuredStep;
    m_stats.calibrated = m_configuredStep != 0;
    m_previousTimestamp = 0;
    m_havePrevious = false;
    m_calibrationTransitions = 0;
    m_stepHistogram.clear();
    m_calibrationTimestamps.clear();
}

void TimestampContinuityAnalyzer::process(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }

    m_leftover.append(bytes);
    const int usableBytes =
        (m_leftover.size() / kFrameBytes) * kFrameBytes;
    if (usableBytes <= 0) {
        return;
    }

    const char *data = m_leftover.constData();
    const int frameCount = usableBytes / kFrameBytes;
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        processFrame(data + frameIndex * kFrameBytes);
    }
    m_leftover.remove(0, usableBytes);
}

TimestampContinuityStats TimestampContinuityAnalyzer::stats() const
{
    return m_stats;
}

void TimestampContinuityAnalyzer::processFrame(const char *frame)
{
    const quint32 timestamp = frameTimestamp(frame);
    const qint64 currentFrame = m_stats.framesSeen;

    if (!m_stats.hasData) {
        m_stats.hasData = true;
        m_stats.firstTimestamp = timestamp;
    }
    m_stats.lastTimestamp = timestamp;
    if (!m_stats.calibrated && m_calibrationTimestamps.isEmpty()) {
        m_calibrationTimestamps.push_back(timestamp);
    }

    // The 256-channel intra-frame scan is the costly part; subsample it.
    const bool doIntraFrame =
        m_intraFrameStride > 0 && (currentFrame % m_intraFrameStride == 0);
    if (doIntraFrame) {
        qint64 mismatchedWords = 0;
        quint32 firstMismatchedTimestamp = timestamp;
        for (int channel = 1; channel < kChannelsTotal; ++channel) {
            const quint32 channelTimestamp =
                frameTimestamp(frame + channel * kBytesPerPoint);
            if (channelTimestamp != timestamp) {
                if (mismatchedWords == 0) {
                    firstMismatchedTimestamp = channelTimestamp;
                }
                ++mismatchedWords;
            }
        }
        if (mismatchedWords > 0) {
            ++m_stats.intraFrameMismatchFrames;
            m_stats.mismatchedChannelWords += mismatchedWords;
            if (m_stats.firstErrorFrame < 0) {
                m_stats.firstErrorFrame = currentFrame;
                m_stats.firstErrorExpected = timestamp;
                m_stats.firstErrorActual = firstMismatchedTimestamp;
            }
        }
    }

    if (m_havePrevious) {
        const quint32 delta =
            (timestamp - m_previousTimestamp) & kTimestampMask;
        if (delta == 0) {
            ++m_stats.repeatedFrames;
        }

        if (!m_stats.calibrated) {
            m_calibrationTimestamps.push_back(timestamp);
            if (delta != 0) {
                ++m_stepHistogram[delta];
            }
            ++m_calibrationTransitions;
            if (m_calibrationTransitions >= kAutoCalibrationTransitions) {
                finishAutoCalibration();
            }
        } else {
            checkTransition(m_previousTimestamp, timestamp, currentFrame);
        }
    }

    m_previousTimestamp = timestamp;
    m_havePrevious = true;
    ++m_stats.framesSeen;
}

void TimestampContinuityAnalyzer::finishAutoCalibration()
{
    quint32 bestStep = 1;
    int bestCount = 0;
    for (auto it = m_stepHistogram.cbegin(); it != m_stepHistogram.cend(); ++it) {
        if (it.value() > bestCount ||
            (it.value() == bestCount && it.key() < bestStep)) {
            bestStep = it.key();
            bestCount = it.value();
        }
    }

    m_stats.expectedStep = bestStep;
    m_stats.calibrated = true;
    m_stepHistogram.clear();
    for (int i = 1; i < m_calibrationTimestamps.size(); ++i) {
        checkTransition(m_calibrationTimestamps[i - 1],
                        m_calibrationTimestamps[i],
                        i);
    }
    m_calibrationTimestamps.clear();
}

void TimestampContinuityAnalyzer::checkTransition(quint32 previousTimestamp,
                                                  quint32 actualTimestamp,
                                                  qint64 frameIndex)
{
    ++m_stats.transitionsChecked;
    const quint32 expected =
        (previousTimestamp + m_stats.expectedStep) & kTimestampMask;
    if (actualTimestamp == expected) {
        return;
    }

    ++m_stats.discontinuities;
    if (m_stats.firstErrorFrame < 0) {
        m_stats.firstErrorFrame = frameIndex;
        m_stats.firstErrorExpected = expected;
        m_stats.firstErrorActual = actualTimestamp;
    }

    const quint32 delta =
        (actualTimestamp - previousTimestamp) & kTimestampMask;
    const quint32 step = m_stats.expectedStep;
    if (delta > step && delta < kTimestampModulo / 2u &&
        step > 0 && delta % step == 0) {
        m_stats.estimatedMissingFrames +=
            static_cast<qint64>(delta / step) - 1;
    } else if (delta != 0) {
        ++m_stats.irregularJumps;
    }
}

TimestampCheckerWorker::TimestampCheckerWorker(
    std::shared_ptr<ThreadSafeQueue<QByteArray>> inputQueue,
    std::atomic_bool *stopFlag,
    int durationMs,
    quint32 expectedStep,
    QObject *parent)
    : QThread(parent),
      m_inputQueue(std::move(inputQueue)),
      m_stopFlag(stopFlag),
      m_durationMs(qMax(1, durationMs)),
      m_expectedStep(expectedStep)
{
    qRegisterMetaType<TimestampContinuityStats>();
}

void TimestampCheckerWorker::run()
{
    TimestampContinuityAnalyzer analyzer(m_expectedStep);
    QElapsedTimer startupTimer;
    QElapsedTimer dataTimer;
    QElapsedTimer reportTimer;
    startupTimer.start();
    reportTimer.start();
    bool durationReached = false;

    while (true) {
        QByteArray chunk;
        const bool gotChunk = m_inputQueue && m_inputQueue->pop(chunk, 100);
        if (gotChunk) {
            analyzer.process(chunk);
            if (analyzer.stats().hasData && !dataTimer.isValid()) {
                dataTimer.start();
            }
        }

        const qint64 elapsedMs = dataTimer.isValid() ? dataTimer.elapsed() : 0;
        if (!dataTimer.isValid() &&
            startupTimer.elapsed() >= kFirstCompleteFrameTimeoutMs) {
            if (m_stopFlag) {
                m_stopFlag->store(true);
            }
            if (m_inputQueue) {
                m_inputQueue->wakeAll();
            }
        }
        if (dataTimer.isValid() && elapsedMs >= m_durationMs) {
            durationReached = true;
            if (m_stopFlag) {
                m_stopFlag->store(true);
            }
            if (m_inputQueue) {
                m_inputQueue->wakeAll();
            }
        }

        if (reportTimer.elapsed() >= kProgressIntervalMs) {
            emit progress(analyzer.stats(), elapsedMs);
            reportTimer.restart();
        }

        const bool stopRequested = m_stopFlag && m_stopFlag->load();
        if (stopRequested && (!m_inputQueue || m_inputQueue->size() == 0)) {
            break;
        }
    }

    const qint64 elapsedMs = dataTimer.isValid() ? dataTimer.elapsed() : 0;
    const TimestampContinuityStats finalStats = analyzer.stats();
    emit progress(finalStats, elapsedMs);
    emit checkFinished(finalStats, elapsedMs, durationReached);
}

TimestampMonitorWorker::TimestampMonitorWorker(
    std::shared_ptr<ThreadSafeQueue<QByteArray>> inputQueue,
    std::atomic_bool *stopFlag,
    quint32 expectedStep,
    int intraFrameStride,
    QObject *parent)
    : QThread(parent),
      m_inputQueue(std::move(inputQueue)),
      m_stopFlag(stopFlag),
      m_intraFrameStride(intraFrameStride),
      m_expectedStep(expectedStep)
{
    qRegisterMetaType<TimestampContinuityStats>();
}

void TimestampMonitorWorker::requestReset(quint32 expectedStep)
{
    m_expectedStep.store(expectedStep);
    m_resetPending.store(true);
}

void TimestampMonitorWorker::run()
{
    TimestampContinuityAnalyzer analyzer(m_expectedStep.load());
    analyzer.setIntraFrameStride(m_intraFrameStride);
    QElapsedTimer reportTimer;
    reportTimer.start();

    while (true) {
        if (m_resetPending.exchange(false)) {
            analyzer.reset(m_expectedStep.load());
            analyzer.setIntraFrameStride(m_intraFrameStride);
            emit statsUpdated(analyzer.stats());
        }

        QByteArray chunk;
        if (m_inputQueue && m_inputQueue->pop(chunk, 200)) {
            analyzer.process(chunk);
        }

        if (reportTimer.elapsed() >= 1000) {
            emit statsUpdated(analyzer.stats());
            reportTimer.restart();
        }

        const bool stopRequested = m_stopFlag && m_stopFlag->load();
        if (stopRequested) {
            break;
        }
    }
    emit statsUpdated(analyzer.stats());
}

}  // namespace ccv2
