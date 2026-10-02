#pragma once

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <memory>

#include "core/threadsafe_queue.h"
#include "io/session_manifest.h"

namespace ccv2 {

class RecordingWorker : public QThread {
    Q_OBJECT
public:
    RecordingWorker(const QString &baseDir,
                    const QString &sessionName,
                    qint64 splitBytes,
                    const SessionMetadata &metadata,
                    QObject *parent = nullptr);
    ~RecordingWorker() override;

    bool startAndWait(int timeoutMs = 5000);
    bool enqueue(const QByteArray &chunk, const StreamBlockInfo &info = {});
    void setIntegrity(const SessionIntegrity &integrity, qint64 ingressDroppedFrames);
    void setAnalysisMetadata(const QJsonObject &metadata);
    bool integrityClean() const;
    qint64 ingressDroppedFrames() const;
    void requestStop(bool complete,
                     const QString &reason,
                     qint64 ingressDroppedFrames);

    QString recordingPath() const;
    qint64 recordedBytes() const { return m_recordedBytes.load(); }
    qint64 droppedFrames() const { return m_droppedFrames.load(); }

signals:
    void recordingFailed(const QString &message);

protected:
    void run() override;

private:
    void failOnce(const QString &message, qint64 droppedBytes = 0);

    QString m_baseDir;
    QString m_sessionName;
    qint64 m_splitBytes{0};
    SessionMetadata m_metadata;
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_queue;

    mutable QMutex m_stateMutex;
    QWaitCondition m_startedCondition;
    QString m_recordingPath;
    QString m_stopReason{QStringLiteral("user_stop")};
    bool m_startReady{false};
    bool m_startOk{false};
    bool m_complete{true};
    qint64 m_ingressDroppedFrames{0};
    SessionIntegrity m_integrity;

    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_accepting{false};
    std::atomic_bool m_failureEmitted{false};
    std::atomic<qint64> m_recordedBytes{0};
    std::atomic<qint64> m_droppedFrames{0};
};

}  // namespace ccv2
