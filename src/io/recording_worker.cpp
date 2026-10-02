#include "io/recording_worker.h"

#include <QDeadlineTimer>

#include "core/constants.h"
#include "io/session_recorder.h"

namespace ccv2 {

namespace {
constexpr int kRecordingQueueChunks = 256;
}

RecordingWorker::RecordingWorker(const QString &baseDir,
                                 const QString &sessionName,
                                 qint64 splitBytes,
                                 const SessionMetadata &metadata,
                                 QObject *parent)
    : QThread(parent),
      m_baseDir(baseDir),
      m_sessionName(sessionName),
      m_splitBytes(splitBytes),
      m_metadata(metadata),
      m_queue(std::make_shared<ThreadSafeQueue<QByteArray>>(kRecordingQueueChunks)) {}

RecordingWorker::~RecordingWorker()
{
    requestStop(false, QStringLiteral("worker_destroyed"), ingressDroppedFrames());
    if (isRunning()) {
        wait();
    }
}

bool RecordingWorker::startAndWait(int timeoutMs)
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_startReady = false;
        m_startOk = false;
    }
    start();
    QDeadlineTimer deadline(timeoutMs);
    QMutexLocker locker(&m_stateMutex);
    // Predicate loop: QWaitCondition can wake spuriously; only a set
    // m_startReady (or the deadline) may end the wait.
    while (!m_startReady && !deadline.hasExpired()) {
        m_startedCondition.wait(&m_stateMutex, deadline);
    }
    return m_startReady && m_startOk;
}

bool RecordingWorker::enqueue(const QByteArray &chunk, const StreamBlockInfo &info)
{
    if (!m_accepting.load() || chunk.isEmpty()) {
        return false;
    }
    if (m_queue->push(chunk, false, nullptr, info)) {
        return true;
    }
    failOnce(QStringLiteral("录制写盘队列已满，录制已停止"),
             chunk.size());
    return false;
}

void RecordingWorker::requestStop(bool complete,
                                  const QString &reason,
                                  qint64 ingressDroppedFrames)
{
    const bool alreadyStopping = m_stopRequested.exchange(true);
    {
        QMutexLocker locker(&m_stateMutex);
        m_complete = m_complete && complete;
        if (!alreadyStopping && !reason.isEmpty()) {
            m_stopReason = reason;
        }
        m_ingressDroppedFrames = ingressDroppedFrames;
    }
    m_accepting.store(false);
    m_queue->wakeAll();
}

QString RecordingWorker::recordingPath() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_recordingPath;
}

void RecordingWorker::setIntegrity(const SessionIntegrity &integrity, qint64 ingressDroppedFrames) {
    QMutexLocker locker(&m_stateMutex);
    m_integrity = integrity;
    m_ingressDroppedFrames = ingressDroppedFrames;
    m_complete = m_complete && integrity.clean() && ingressDroppedFrames == 0;
}

bool RecordingWorker::integrityClean() const {
    QMutexLocker locker(&m_stateMutex);
    return m_integrity.clean() && m_ingressDroppedFrames == 0;
}

qint64 RecordingWorker::ingressDroppedFrames() const {
    QMutexLocker locker(&m_stateMutex);
    return m_ingressDroppedFrames;
}

void RecordingWorker::failOnce(const QString &message, qint64 droppedBytes)
{
    if (droppedBytes > 0) {
        m_droppedFrames.fetch_add(droppedBytes / kFrameBytes);
    }
    {
        QMutexLocker locker(&m_stateMutex);
        m_complete = false;
        m_stopReason = message;
    }
    m_accepting.store(false);
    m_stopRequested.store(true);
    m_queue->wakeAll();
    if (!m_failureEmitted.exchange(true)) {
        emit recordingFailed(message);
    }
}

void RecordingWorker::run()
{
    SessionRecorder recorder;
    const QString path =
        recorder.start(m_baseDir, m_sessionName, m_splitBytes, m_metadata);
    if (!path.isEmpty()) {
        m_accepting.store(true);
    }
    {
        QMutexLocker locker(&m_stateMutex);
        m_recordingPath = path;
        m_startOk = !path.isEmpty();
        m_startReady = true;
        m_startedCondition.wakeAll();
    }
    if (path.isEmpty()) {
        failOnce(QStringLiteral("无法创建录制目录或文件"));
        return;
    }

    bool firstChunk = true;
    while (true) {
        QByteArray chunk;
        StreamBlockInfo info;
        if (m_queue->pop(chunk, 100, nullptr, &info)) {
            if (firstChunk && info.valid() && !recorder.setFrameOrigin(info.firstFrame())) {
                failOnce(QStringLiteral("录制起始帧元数据写入失败"));
                break;
            }
            firstChunk = false;
            if (!recorder.write(chunk)) {
                failOnce(QStringLiteral("ADC 原始流写入失败"));
                break;
            }
            m_recordedBytes.fetch_add(chunk.size());
        }
        if (m_stopRequested.load() && m_queue->size() == 0) {
            break;
        }
    }

    bool complete = true;
    QString reason;
    qint64 ingressDropped = 0;
    SessionIntegrity integrity;
    {
        QMutexLocker locker(&m_stateMutex);
        complete = m_complete && !m_failureEmitted.load();
        reason = m_stopReason;
        ingressDropped = m_ingressDroppedFrames;
        integrity = m_integrity;
    }
    if (!recorder.stop(complete,
                       reason,
                       ingressDropped,
                        m_droppedFrames.load(), integrity)) {
        failOnce(QStringLiteral("录制文件已关闭，但 session.json 最终状态写入失败"));
    }
    m_accepting.store(false);
}

}  // namespace ccv2
