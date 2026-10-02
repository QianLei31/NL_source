#include "service/session_hub.h"

#include <algorithm>

#include <QDateTime>
#include <QJsonArray>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include "core/constants.h"
#include "core/tdm_context.h"
#include "io/recording_worker.h"
#include "network/replay_controller.h"
#include "network/socket_receiver2.h"

namespace ccv2 {

class SessionDistributor : public QThread {
public:
    SessionDistributor(SessionHub *hub,
                       std::shared_ptr<ThreadSafeQueue<QByteArray>> queue,
                       std::atomic_bool *stopFlag)
        : m_hub(hub), m_queue(std::move(queue)), m_stopFlag(stopFlag) {}

protected:
    void run() override {
        while (true) {
            const bool dispatched = m_hub->dispatchNext(m_queue);
            if (m_stopFlag->load() && m_queue->size() == 0) {
                break;
            }
            // A live source normally keeps the queue non-empty. Yield between
            // batches so GUI subscription/removal is never starved by this
            // tight distributor loop.
            if (dispatched) {
                QThread::yieldCurrentThread();
            }
        }
    }

private:
    SessionHub *m_hub;
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_queue;
    std::atomic_bool *m_stopFlag;
};

SessionHub::SessionHub(QObject *parent)
    : QObject(parent),
      m_hubQueue(std::make_shared<ThreadSafeQueue<QByteArray>>(256)),
      m_stopFlag(std::make_unique<std::atomic_bool>(false)),
      m_tdmContext(new TdmContext(this)) {}

SessionHub::~SessionHub()
{
    stopInternal(State::Idle, QStringLiteral("application_close"));
    cleanupRecordingWorker();
}

bool SessionHub::isRunning() const
{
    const State current = state();
    return current == State::ConnectingLive ||
           current == State::Live ||
           current == State::ReplayReady ||
           current == State::ReplayPlaying ||
           current == State::ReplayPaused;
}

bool SessionHub::isReplaying() const
{
    const State current = state();
    return current == State::ReplayReady ||
           current == State::ReplayPlaying ||
           current == State::ReplayPaused;
}

qint64 SessionHub::currentFrameIndex() const
{
    return m_currentFrameIndex.load();
}

void SessionHub::setState(State next)
{
    const State previous =
        static_cast<State>(m_state.exchange(static_cast<int>(next)));
    if (previous != next) {
        emit stateChanged(next);
    }
}

void SessionHub::addSubscriber(
    const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue)
{
    addSubscriberWithFrameOrigin(queue);
}

qint64 SessionHub::addSubscriberWithFrameOrigin(
    const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue)
{
    if (!queue) return m_timelineFrameOrigin.load();
    queue->clear();
    QMutexLocker subscriberLocker(&m_subMutex);
    if (!m_subscribers.contains(queue)) {
        m_subscribers.push_back(queue);
    }
    // dispatchNext updates this counter while holding the same subscriber
    // mutex, so the returned origin is exactly the first frame that can be
    // enqueued for this new subscriber. No long-lived dispatch lock is needed.
    return m_currentFrameIndex.load();
}

void SessionHub::removeSubscriber(
    const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue)
{
    QMutexLocker locker(&m_subMutex);
    m_subscribers.removeAll(queue);
    if (queue) queue->clear();
}

bool SessionHub::start(const QString &host,
                       int controlPort,
                       int dataPort,
                       const QString &cmd,
                       double sampleRate)
{
    if (isRunning() || state() == State::Stopping) {
        return false;
    }
    if (state() == State::Error) {
        stopInternal(State::Idle, QStringLiteral("error_reset"));
    }

    resetTimeline(0);
    m_stopFlag = std::make_unique<std::atomic_bool>(false);
    m_hubQueue->clear();
    m_host = host;
    m_controlPort = controlPort;
    m_dataPort = dataPort;
    m_command = cmd;
    m_sourceType = QStringLiteral("live");
    m_sampleRate.store(qMax(1.0, sampleRate));
    m_connected.store(false);
    m_receivedBytes.store(0);
    m_distributedFrames.store(0);
    m_ingressDroppedFrames.store(0);
    m_subscriberDroppedFrames.store(0);
    m_triggerPrimed = false;  // distributor not started yet — safe to touch

    if (controlPort == dataPort) {
        m_receiver =
            new SocketReceiver2(host, controlPort, m_hubQueue, m_stopFlag.get(), this);
    } else {
        m_receiver = new SocketReceiver2(
            host, controlPort, dataPort, m_hubQueue, m_stopFlag.get(), this);
    }
    if (!cmd.isEmpty()) {
        m_receiver->setCommand(cmd);
    }
    const quint64 receiverEpoch = timelineEpoch();

    connect(m_receiver,
            &SocketReceiver2::controlConnectionEstablished,
            this,
            [this, receiverEpoch](const QString &message) {
        if (timelineEpoch() != receiverEpoch) return;
        emit controlLinkResult(true, message);
    });
    connect(m_receiver,
            &SocketReceiver2::connectionEstablished,
            this,
            [this, receiverEpoch](const QString &message) {
        if (timelineEpoch() != receiverEpoch) return;
        m_connected.store(true);
        setState(State::Live);
        emit connectionStateChanged(true);
        emit connectionEvent(message);
    });
    connect(m_receiver,
            &SocketReceiver2::connectionError,
            this,
            [this, receiverEpoch](const QString &message) {
        if (timelineEpoch() != receiverEpoch) return;
        const bool controlFailure =
            message.contains(QStringLiteral("control port"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("stream command"), Qt::CaseInsensitive) ||
            (message.startsWith(QStringLiteral("Failed to connect "), Qt::CaseInsensitive) &&
             !message.contains(QStringLiteral("data port"), Qt::CaseInsensitive));
        if (controlFailure) {
            emit controlLinkResult(false, message);
        }
        emit connectionEvent(message);
        QTimer::singleShot(0, this, [this, message, receiverEpoch]() {
            if (timelineEpoch() == receiverEpoch &&
                (state() == State::ConnectingLive || state() == State::Live)) {
                stopInternal(State::Error, message);
            }
        });
    });
    // Direct connection: runs on the receiver thread, but only touches an
    // atomic. Queued delivery would lag behind when the GUI thread is busy,
    // skewing the toolbar's rate readout.
    connect(m_receiver, &SocketReceiver2::bytesReceived, this, [this, receiverEpoch](int bytes) {
        if (timelineEpoch() != receiverEpoch) return;
        m_receivedBytes.fetch_add(qMax(0, bytes));
    }, Qt::DirectConnection);
    connect(m_receiver, &SocketReceiver2::framesDropped, this, [this, receiverEpoch](qint64 frames) {
        if (timelineEpoch() != receiverEpoch) return;
        const qint64 safeFrames = qMax<qint64>(0, frames);
        m_ingressDroppedFrames.fetch_add(safeFrames);
        emit framesDropped(safeFrames);
        emit statisticsChanged();
    }, Qt::DirectConnection);
    connect(m_receiver, &QThread::finished, this, [this, receiverEpoch]() {
        if (timelineEpoch() != receiverEpoch) return;
        if ((state() == State::ConnectingLive || state() == State::Live) &&
            m_stopFlag && !m_stopFlag->load()) {
            stopInternal(State::Error, QStringLiteral("数据接收线程意外结束"));
        }
    });

    m_distributorStop.store(false);
    m_distributor =
        new SessionDistributor(this, m_hubQueue, &m_distributorStop);
    m_distributor->start();
    setState(State::ConnectingLive);
    m_receiver->start();
    return true;
}

bool SessionHub::startReplay(const QString &binFile, double fallbackSampleRate)
{
    stopInternal(State::Idle, QStringLiteral("source_change"));
    resetTimeline(0);

    m_stopFlag = std::make_unique<std::atomic_bool>(false);
    m_hubQueue->clear();
    m_sourceType = QStringLiteral("replay");
    m_receivedBytes.store(0);
    m_distributedFrames.store(0);
    m_ingressDroppedFrames.store(0);
    m_subscriberDroppedFrames.store(0);

    m_replay = new ReplayController(m_hubQueue, this);
    if (!m_replay->open(binFile, fallbackSampleRate)) {
        const QString replayError = m_replay->errorString();
        delete m_replay;
        m_replay = nullptr;
        setState(State::Error);
        emit connectionEvent(
            QStringLiteral("回放打开失败: %1")
                .arg(replayError.isEmpty() ? binFile : replayError));
        return false;
    }
    m_sampleRate.store(m_replay->sampleRate());
    resetTimeline(m_replay->nextSourceFrame());
    m_replay->setTimelineEpoch(timelineEpoch());
    connect(m_replay, &ReplayController::seekStarted, this, [this](qint64 target) {
        resetTimeline(target);
    });
    connect(m_replay, &ReplayController::timelinePositioned, this, [this](qint64 origin) {
        resetTimeline(origin);
        if (m_replay) m_replay->setTimelineEpoch(timelineEpoch());
    });
    ReplayController *const activeReplay = m_replay;
    connect(m_replay, &ReplayController::errorOccurred, this, [this, activeReplay](const QString &error) {
        QTimer::singleShot(0, this, [this, activeReplay, error]() {
            if (m_replay != activeReplay) return;
            stopInternal(State::Error, error);
            emit replayError(error);
        });
    });
    // A session records the TDM state it was captured with; replaying it must
    // restore that state so electrode identities match the original run.
    // Single-BIN replays and pre-TDM manifests leave the current state alone.
    if (m_replay->tdmKnown()) {
        m_tdmContext->setState(m_replay->tdmEnabled(), m_replay->tdmEvenFirst());
    }
    QJsonObject replayAnalysis = m_replay->metadata().neuralAnalysis;
    replayAnalysis["source_provenance"] = m_replay->sourceProvenance();
    emit replayAnalysisMetadataAvailable(replayAnalysis);
    connect(m_replay,
            &ReplayController::positionChanged,
            this,
            &SessionHub::replayPosition);
    connect(m_replay,
            &ReplayController::playingChanged,
            this,
            [this](bool playing) {
        if (isReplaying()) {
            setState(playing ? State::ReplayPlaying : State::ReplayPaused);
        }
        emit replayPlayingChanged(playing);
    });
    connect(m_replay,
            &ReplayController::backpressureChanged,
            this,
            &SessionHub::replayBackpressureChanged);
    connect(m_replay, &ReplayController::finished, this, [this]() {
        setState(State::ReplayReady);
        emit replayFinished();
    });

    m_distributorStop.store(false);
    m_distributor =
        new SessionDistributor(this, m_hubQueue, &m_distributorStop);
    m_distributor->start();
    m_connected.store(true);
    setState(State::ReplayReady);
    emit connectionStateChanged(true);
    emit replayPosition(m_replay->currentFrame(), m_replay->totalFrames());
    emit replayPlayingChanged(false);
    emit connectionEvent(QStringLiteral("回放就绪(暂停): %1").arg(binFile));
    if (!m_replay->warning().isEmpty()) {
        emit connectionEvent(QStringLiteral("回放警告: %1").arg(m_replay->warning()));
    }
    return true;
}

void SessionHub::replayTogglePlay()
{
    if (m_replay) m_replay->togglePlay();
}

void SessionHub::replayPause()
{
    if (m_replay) m_replay->pause();
}

void SessionHub::performReplaySeek(qint64 targetFrame)
{
    if (!m_replay) return;
    const bool wasPlaying = m_replay->isPlaying();
    m_replay->pause();
    m_replay->seekFrame(targetFrame);
    if (wasPlaying) {
        m_replay->play();
    } else {
        setState(State::ReplayPaused);
    }
}

void SessionHub::replayJumpStart()
{
    performReplaySeek(0);
}

void SessionHub::replayJumpEnd()
{
    if (m_replay) performReplaySeek(m_replay->totalFrames());
}

void SessionHub::replaySkip(double seconds)
{
    if (!m_replay) return;
    const qint64 target =
        m_replay->currentFrame() +
        static_cast<qint64>(seconds * m_replay->sampleRate());
    performReplaySeek(qBound<qint64>(0, target, m_replay->totalFrames()));
}

void SessionHub::replaySeekFraction(double fraction)
{
    if (!m_replay) return;
    const double bounded = qBound(0.0, fraction, 1.0);
    performReplaySeek(
        static_cast<qint64>(bounded * m_replay->totalFrames()));
}

void SessionHub::resetTimeline(qint64 targetFrame, bool notifyViews)
{
    quint64 epoch = 0;
    {
        QMutexLocker dispatchLocker(&m_dispatchMutex);
        m_hubQueue->clear();
        QMutexLocker subscriberLocker(&m_subMutex);
        for (const auto &queue : m_subscribers) {
            if (queue) queue->clear();
        }
        const qint64 boundedTarget = qMax<qint64>(0, targetFrame);
        m_currentFrameIndex.store(boundedTarget);
        m_timelineFrameOrigin.store(boundedTarget);
        epoch = m_timelineEpoch.fetch_add(1) + 1;
        // The timestamp counter is not continuous across a seek/source change.
        m_sourceTimeline.reset(boundedTarget);
        m_integrityAnalyzer.reset(0);
        m_integrity = {};
        m_timestampCounterObserved = false;
        m_upstreamMissingFrames.store(0);
        m_triggerPrimed = false;
    }
    // notifyViews=false keeps the last frames frozen on screen (session stop);
    // starts and seeks notify so pages clear before the new timeline arrives.
    if (notifyViews) {
        emit timelineReset(epoch, targetFrame);
    }
}

bool SessionHub::dispatchNext(
    const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue)
{
    // Waiting for ingress data must not hold m_dispatchMutex. Page activation
    // takes that mutex briefly to atomically subscribe at a frame boundary;
    // holding it across pop() starves the GUI under a continuous live stream.
    const quint64 poppedEpoch = m_timelineEpoch.load();
    QByteArray chunk;
    qint64 droppedBytesBefore = 0;
    StreamBlockInfo sourceInfo;
    if (!queue->pop(chunk, 25, &droppedBytesBefore, &sourceInfo)) {
        return false;
    }
    if (chunk.isEmpty()) {
        return false;
    }

    QMutexLocker dispatchLocker(&m_dispatchMutex);
    // A replay seek/reset may have happened after pop() but before this lock.
    // Discard the pre-reset chunk instead of injecting it into the new epoch.
    if ((sourceInfo.valid() ? sourceInfo.epoch : poppedEpoch) != m_timelineEpoch.load()) {
        return false;
    }
    onChunkLocked(chunk, droppedBytesBefore, sourceInfo);
    return true;
}

void SessionHub::checkTrigger(const QByteArray &chunk, const StreamBlockInfo &info)
{
    if (m_sourceType != QStringLiteral("live") || state() != State::Live) {
        m_triggerPrimed = false;
        return;
    }
    TriggerConfig cfg;
    {
        QMutexLocker locker(&m_triggerMutex);
        cfg = m_trigger;
    }
    // Only arm while enabled and idle; recording (or its absence) IS the arm
    // state, so a fired trigger won't re-fire until the current capture ends.
    if (!cfg.enabled || m_recording.load()) {
        m_triggerPrimed = false;
        return;
    }
    const int ch = std::clamp(cfg.channel, 0, kChannelsTotal - 1);
    const int frames = static_cast<int>(chunk.size() / kFrameBytes);
    const uchar *base = reinterpret_cast<const uchar *>(chunk.constData());
    // In TDM mode a single ADC channel interleaves four electrodes with
    // different DC levels; evaluating every frame would let the level step
    // between them repeatedly cross the threshold. Restrict crossing detection
    // to the chosen electrode's frames so the trigger tracks one electrode.
    const bool tdmActive =
        m_tdmContext && m_tdmContext->enabled() && cfg.tdmSlot >= 0;
    const bool pair02 = !m_tdmContext || m_tdmContext->pair02();
    for (int f = 0; f < frames; ++f) {
        const uchar *frame = base + static_cast<qsizetype>(f) * kFrameBytes;
        if (tdmActive) {
            if (!triggerFrameMatchesSlot(info.frameIndices[f], pair02, cfg.tdmSlot)) {
                continue;  // other electrode: leaves prev value untouched
            }
        }
        const quint32 raw =
            qFromLittleEndian<quint32>(frame + ch * kBytesPerPoint);
        const double v = static_cast<double>(raw & kAdcSampleMask) / 4096.0 * 1.8;
        bool crossed = false;
        if (m_triggerPrimed) {
            crossed = cfg.risingAbove
                          ? (m_triggerPrevValue <= cfg.thresholdV && v > cfg.thresholdV)
                          : (m_triggerPrevValue >= cfg.thresholdV && v < cfg.thresholdV);
        }
        m_triggerPrevValue = v;
        m_triggerPrimed = true;
        if (crossed) {
            emit triggerFired(info.epoch);
            return;
        }
    }
}

void SessionHub::onChunkLocked(const QByteArray &chunk, qint64 droppedBytesBefore, const StreamBlockInfo &sourceInfo)
{
    const qint64 frames = chunk.size() / kFrameBytes;
    if (frames <= 0 || chunk.size() % kFrameBytes != 0) return;
    StreamBlockInfo info = sourceInfo;
    if (info.valid() && info.frameIndices.size() != frames) return;
    if (m_sourceType == QStringLiteral("live") && !info.integrityUnknown && info.frameValid.isEmpty()) {
        info.frameValid.fill(true, static_cast<qsizetype>(frames));
        for (qint64 f = 0; f < frames; ++f) {
            const char *frame = chunk.constData() + f * kFrameBytes;
            const quint32 ts = qFromLittleEndian<quint32>(frame) >> kTimestampShift;
            for (int ch = 1; ch < kChannelsTotal; ++ch) {
                if ((qFromLittleEndian<quint32>(frame + ch * kBytesPerPoint) >> kTimestampShift) != ts) {
                    info.frameValid[static_cast<qsizetype>(f)] = false;
                    break;
                }
            }
        }
    }
    if (!info.valid()) {
        info.epoch = m_timelineEpoch.load();
        m_sourceTimeline.skipFrames(droppedBytesBefore / kFrameBytes);
        info.frameIndices.reserve(static_cast<qsizetype>(frames));
        for (qint64 f = 0; f < frames; ++f) {
            const quint32 ts = qFromLittleEndian<quint32>(chunk.constData() + f * kFrameBytes) >> kTimestampShift;
            if (!info.frameValid.isEmpty() && !info.frameValid[static_cast<qsizetype>(f)]) {
                info.frameIndices.push_back(m_sourceTimeline.nextIndex());
                m_sourceTimeline.skipFrames(1);
            } else {
                info.frameIndices.push_back(m_sourceTimeline.advance(ts));
            }
        }
    }
    const auto before = m_integrityAnalyzer.stats();
    if (!info.integrityUnknown) m_integrityAnalyzer.process(chunk);
    const auto after = m_integrityAnalyzer.stats();
    m_integrity.upstreamMissingFrames += qMax<qint64>(0,
        after.estimatedMissingFrames - before.estimatedMissingFrames -
        (before.hasData ? droppedBytesBefore / kFrameBytes : 0));
    // Constant legacy timestamps carry no continuity evidence. Once a real
    // counter is observed, retain its errors, including later repetitions.
    m_timestampCounterObserved = m_timestampCounterObserved || after.firstTimestamp != after.lastTimestamp ||
        (before.hasData && before.lastTimestamp != after.lastTimestamp);
    if (m_timestampCounterObserved) {
        m_integrity.repeatedFrames = after.repeatedFrames;
        m_integrity.irregularJumps = after.irregularJumps;
    }
    m_integrity.intraFrameMismatchFrames = after.intraFrameMismatchFrames;
    m_upstreamMissingFrames.store(m_integrity.upstreamMissingFrames);
    m_distributedFrames.fetch_add(frames);

    checkTrigger(chunk, info);

    if (m_recording.load() && m_recordWorker) {
        // Frames evicted from the hub queue never reached the recorder. Write
        // an equal run of zero frames first so the recorded file stays
        // continuous with wall-clock time and its internal frame parity (and
        // thus TDM electrode identity on replay/export) matches the source.
        // The lost-frame count is reported separately via ingress statistics.
        const qint64 droppedFrames = droppedBytesBefore / kFrameBytes;
        if (droppedFrames > 0) {
            StreamBlockInfo paddingInfo;
            paddingInfo.epoch = info.epoch;
            paddingInfo.frameValid.fill(false, static_cast<qsizetype>(droppedFrames));
            const qint64 firstMissing = qMax<qint64>(0, info.firstFrame() - droppedFrames);
            paddingInfo.frameIndices.reserve(static_cast<qsizetype>(droppedFrames));
            for (qint64 f = 0; f < droppedFrames; ++f) paddingInfo.frameIndices.push_back(firstMissing + f);
            m_recordWorker->enqueue(QByteArray(droppedFrames * kFrameBytes, '\0'), paddingInfo);
        }
        m_recordWorker->enqueue(chunk, info);
    }

    qint64 subscriberDrops = 0;
    QMutexLocker subscriberLocker(&m_subMutex);
    for (const auto &queue : m_subscribers) {
        if (!queue) continue;
        QByteArray dropped;
        if (!queue->push(chunk, true, &dropped, info)) {
            subscriberDrops += dropped.size() / kFrameBytes;
        }
    }
    if (subscriberDrops > 0) {
        m_subscriberDroppedFrames.fetch_add(subscriberDrops);
        emit statisticsChanged();
    }
    // Keep frame-origin publication atomic with subscriber delivery. A page
    // added before this block receives the chunk and gets its start index; a
    // page added after it starts at the following frame.
    m_currentFrameIndex.store(info.nextFrame());
}

void SessionHub::setReferenceMode(int mode) {
    mode = qBound(0, mode, 2);
    if (m_referenceMode.exchange(mode) == mode) return;
    QJsonObject metadata = analysisMetadata();
    if (!metadata.isEmpty()) {
        metadata["reference_mode"] = mode;
        setAnalysisMetadata(metadata);
    }
    emit referenceModeChanged(mode);
}

QJsonObject SessionHub::analysisMetadata() const {
    QMutexLocker locker(&m_dispatchMutex);
    return m_analysisMetadata;
}

void SessionHub::setAnalysisMetadata(const QJsonObject &metadata) {
    QMutexLocker locker(&m_dispatchMutex);
    if (m_analysisMetadata == metadata) return;
    m_analysisMetadata = metadata;
    if (m_recording.load() && m_recordWorker) {
        QJsonArray changes = m_recordingAnalysisMetadata.value("configuration_changes").toArray();
        if (changes.size() < 256) {
            QJsonObject change;
            change["requested_source_frame"] = QString::number(m_currentFrameIndex.load());
            change["configuration"] = metadata;
            changes.append(change);
            m_recordingAnalysisMetadata["configuration_changes"] = changes;
        } else {
            m_recordingAnalysisMetadata["history_truncated"] = true;
        }
        m_recordingAnalysisMetadata["configuration_changed"] = true;
        m_recordWorker->setAnalysisMetadata(m_recordingAnalysisMetadata);
    }
}

bool SessionHub::startRecording(const QString &baseDir,
                                const QString &sessionName,
                                qint64 splitBytes)
{
    if (m_recording.load()) return true;
    cleanupRecordingWorker();

    SessionMetadata metadata;
    metadata.source = m_sourceType;
    metadata.host = m_host;
    metadata.controlPort = m_controlPort;
    metadata.dataPort = m_dataPort;
    metadata.command = m_command;
    metadata.sampleRate = sampleRate();
    m_recordingAnalysisMetadata = QJsonObject{
        {"schema_version", 1}, {"initial_configuration", analysisMetadata()},
        {"configuration_changes", QJsonArray{}}, {"configuration_changed", false},
        {"history_truncated", false},
        {"change_time_semantics", "requested_source_frame_not_atomic_detector_application"}};
    metadata.neuralAnalysis = m_recordingAnalysisMetadata;
    metadata.tdmKnown = true;
    metadata.tdmEnabled = m_tdmContext->enabled();
    metadata.tdmEvenFirst = m_tdmContext->pair02();
    metadata.createdAt =
        QDateTime::currentDateTime().toString(Qt::ISODate);

    RecordingWorker *worker =
        new RecordingWorker(baseDir, sessionName, splitBytes, metadata, this);
    connect(worker,
            &RecordingWorker::recordingFailed,
            this,
            [this, worker](const QString &message) {
        bool wasRecording = false;
        {
            // A queued failure can be delivered after this worker was already
            // detached (stopRecording) and a new session started; comparing
            // the captured pointer (never dereferenced) under the same mutex
            // keeps the stale event from stopping the new session.
            QMutexLocker dispatchLocker(&m_dispatchMutex);
            if (m_recordWorker != worker) {
                return;
            }
            wasRecording = m_recording.exchange(false);
        }
        if (wasRecording) {
            emit recordingStateChanged(false, m_recordingPath);
        }
        emit recordingError(message);
    });
    if (!worker->startAndWait()) {
        const QString path = worker->recordingPath();
        worker->requestStop(false,
                            QStringLiteral("recording_start_failed"),
                            m_ingressDroppedFrames.load());
        if (!worker->wait(2000)) {
            emit recordingError(QStringLiteral("录制线程停止超时，正在等待安全收尾"));
            worker->wait();
        }
        delete worker;
        m_recordingPath = path;
        return false;
    }

    m_recordingPath = worker->recordingPath();
    m_lastRecordedBytes.store(0);
    {
        // Publish under m_dispatchMutex so the distributor never observes a
        // half-initialized worker (see detachRecordingWorker for the teardown
        // side of this contract).
        QMutexLocker dispatchLocker(&m_dispatchMutex);
        m_recordWorker = worker;
        m_recordingIntegrityBase = m_integrity;
        m_recordingIngressBase = m_ingressDroppedFrames.load();
        m_recording.store(true);
    }
    emit recordingStateChanged(true, m_recordingPath);
    return true;
}

void SessionHub::setTriggerConfig(const TriggerConfig &cfg)
{
    QMutexLocker locker(&m_triggerMutex);
    m_trigger = cfg;
}

RecordingWorker *SessionHub::detachRecordingWorker(bool *wasRecording)
{
    // onChunkLocked() dereferences m_recordWorker on the distributor thread
    // while holding m_dispatchMutex. Detach the pointer under the same mutex
    // so stop/wait/delete below can never race an in-flight enqueue().
    QMutexLocker dispatchLocker(&m_dispatchMutex);
    const bool recording = m_recording.exchange(false);
    if (wasRecording) {
        *wasRecording = recording;
    }
    RecordingWorker *worker = m_recordWorker;
    if (worker) {
        const auto integrity = recordingIntegrityLocked();
        const qint64 dropped = qMax<qint64>(0, m_ingressDroppedFrames.load() - m_recordingIngressBase);
        worker->setIntegrity(integrity, dropped);
    }
    m_recordWorker = nullptr;
    return worker;
}

void SessionHub::stopRecording()
{
    bool wasRecording = false;
    RecordingWorker *worker = detachRecordingWorker(&wasRecording);
    if (worker) {
        const bool complete =
            worker->integrityClean() &&
            worker->droppedFrames() == 0;
        worker->requestStop(
            complete,
            complete ? QStringLiteral("user_stop")
                     : QStringLiteral("stream_incomplete"),
            worker->ingressDroppedFrames());
        if (!worker->wait(10000)) {
            emit recordingError(QStringLiteral("录制线程停止超时，正在等待安全收尾"));
            worker->wait();
        }
        m_lastRecordedBytes.store(worker->recordedBytes());
        m_recordingPath = worker->recordingPath();
        delete worker;
    }
    if (wasRecording) {
        emit recordingStateChanged(false, m_recordingPath);
    }
}

void SessionHub::cleanupRecordingWorker()
{
    RecordingWorker *worker = detachRecordingWorker(nullptr);
    if (!worker) return;
    if (worker->isRunning()) {
        worker->requestStop(false,
                            QStringLiteral("recording_replaced"),
                            m_ingressDroppedFrames.load());
        worker->wait();
    }
    m_lastRecordedBytes.store(worker->recordedBytes());
    delete worker;
}

qint64 SessionHub::recordedBytes() const
{
    return m_recordWorker ? m_recordWorker->recordedBytes()
                          : m_lastRecordedBytes.load();
}

QString SessionHub::recordingPath() const
{
    return m_recordingPath;
}

SessionHub::Statistics SessionHub::statistics() const
{
    Statistics result;
    result.receivedBytes = m_receivedBytes.load();
    result.distributedFrames = m_distributedFrames.load();
    result.ingressDroppedFrames = m_ingressDroppedFrames.load();
    result.subscriberDroppedFrames = m_subscriberDroppedFrames.load();
    result.recordedBytes = recordedBytes();
    result.upstreamMissingFrames = m_upstreamMissingFrames.load();
    return result;
}

void SessionHub::stop()
{
    stopInternal(State::Idle, QStringLiteral("user_stop"));
}

void SessionHub::stopInternal(State finalState, const QString &reason)
{
    const bool hadSource = isRunning() || state() == State::Stopping;
    const bool wasConnected = m_connected.exchange(false);
    if (hadSource) {
        setState(State::Stopping);
    }

    if (m_replay) {
        m_replay->close();
        delete m_replay;
        m_replay = nullptr;
    }

    if (m_stopFlag) {
        m_stopFlag->store(true);
    }
    if (m_hubQueue) {
        m_hubQueue->wakeAll();
    }

    if (m_receiver) {
        if (!m_receiver->wait(5000)) {
            emit connectionEvent(
                QStringLiteral("接收线程停止较慢，正在等待安全退出"));
            m_receiver->wait();
        }
        delete m_receiver;
        m_receiver = nullptr;
    }
    if (m_distributor) {
        // The producer is joined: drain every received complete frame before
        // stopping the recorder, including the receiver's final partial batch.
        m_distributorStop.store(true);
        if (m_hubQueue) m_hubQueue->wakeAll();
        if (!m_distributor->wait(5000)) {
            emit connectionEvent(
                QStringLiteral("分发线程停止较慢，正在等待安全退出"));
            m_distributor->wait();
        }
        delete m_distributor;
        m_distributor = nullptr;
    }

    if (m_recording.load() || m_recordWorker) {
        if (m_recordWorker) {
            m_recordWorker->setIntegrity(recordingIntegrityLocked(),
                qMax<qint64>(0, m_ingressDroppedFrames.load() - m_recordingIngressBase));
            const bool complete =
                finalState != State::Error &&
                m_recordWorker->integrityClean() &&
                m_recordWorker->droppedFrames() == 0;
            m_recordWorker->requestStop(
                complete,
                complete ? reason : QStringLiteral("session_incomplete"),
                m_recordWorker->ingressDroppedFrames());
        }
        stopRecording();
    }

    // Silent reset: flush queues and invalidate in-flight chunks, but leave
    // the pages' last frames on screen (Intan-style freeze on stop).
    resetTimeline(0, false);
    if (wasConnected || hadSource) {
        emit connectionStateChanged(false);
    }
    setState(finalState);
}

SessionIntegrity SessionHub::recordingIntegrityLocked() const {
    SessionIntegrity delta;
    delta.upstreamMissingFrames = qMax<qint64>(0, m_integrity.upstreamMissingFrames - m_recordingIntegrityBase.upstreamMissingFrames);
    delta.repeatedFrames = qMax<qint64>(0, m_integrity.repeatedFrames - m_recordingIntegrityBase.repeatedFrames);
    delta.irregularJumps = qMax<qint64>(0, m_integrity.irregularJumps - m_recordingIntegrityBase.irregularJumps);
    delta.intraFrameMismatchFrames = qMax<qint64>(0, m_integrity.intraFrameMismatchFrames - m_recordingIntegrityBase.intraFrameMismatchFrames);
    return delta;
}

}  // namespace ccv2
