#pragma once

#include <QByteArray>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "core/frame_timestamp_reconciler.h"
#include "core/threadsafe_queue.h"

namespace ccv2 {

class DataStore;
class SessionRecorder;

class SorterWorker : public QThread {
    Q_OBJECT

public:
    // timelineEpoch (optional): hub generation counter; stale chunks from a
    // pre-seek timeline are dropped instead of being appended to the store.
    SorterWorker(std::shared_ptr<ThreadSafeQueue<QByteArray>> inputQueue,
                 DataStore *dataStore,
                 const QVector<int> &selectedChannels,
                 SessionRecorder *recorder,
                 std::function<bool()> saveEnabledFn,
                 std::function<bool()> processEnabledFn,
                 std::atomic_bool *stopFlag,
                 const std::atomic<quint64> *timelineEpoch = nullptr,
                 QObject *parent = nullptr);

signals:
    void framesParsed(int frameCount, int leftoverBytes);
    void recordingError(const QString &message);

protected:
    void run() override;

private:
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_inputQueue;
    DataStore *m_dataStore;
    QVector<int> m_selectedChannels;
    SessionRecorder *m_recorder;
    std::function<bool()> m_saveEnabledFn;
    std::function<bool()> m_processEnabledFn;
    std::atomic_bool *m_stopFlag;
    const std::atomic<quint64> *m_timelineEpoch{nullptr};
    QByteArray m_incomplete;
    bool m_recordingFailed{false};
    quint64 m_lastEpoch{0};
    // Recovers the true frame count from the hardware timestamp so an upstream
    // drop advances the store's index (keeping TDM parity) rather than shifting
    // it. Dormant on zero-timestamp streams.
    FrameTimestampReconciler m_tsRecon;
};

}  // namespace ccv2
