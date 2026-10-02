#pragma once

#include <QByteArray>
#include <QThread>

#include <atomic>
#include <memory>

#include "core/threadsafe_queue.h"

namespace ccv2 {

/// Dual-socket receiver:
///   - Connects to controlPort (7) FIRST to send commands (ctre/stop)
///   - Then connects to dataPort (5001) to receive continuous DMA data
/// Also supports legacy single-port mode for backward compat.
class SocketReceiver2 : public QThread {
    Q_OBJECT

public:
    /// Legacy single-port constructor
    SocketReceiver2(const QString &host, int port,
                    std::shared_ptr<ThreadSafeQueue<QByteArray>> outputQueue,
                    std::atomic_bool *stopFlag, QObject *parent = nullptr);

    /// Dual-port constructor (preferred)
    SocketReceiver2(const QString &host, int controlPort, int dataPort,
                    std::shared_ptr<ThreadSafeQueue<QByteArray>> outputQueue,
                    std::atomic_bool *stopFlag, QObject *parent = nullptr);

    void setCommand(const QString &cmd) { m_command = cmd; }

signals:
    void connectionError(const QString &message);
    void controlConnectionEstablished(const QString &message);
    void connectionEstablished(const QString &message);
    void bytesReceived(int bytes);
    void framesDropped(qint64 frames);

protected:
    void run() override;

private:
    QString m_host;
    int m_controlPort;
    int m_dataPort;
    bool m_dualMode;
    QString m_command{QStringLiteral("ctre")};
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_outputQueue;
    std::atomic_bool *m_stopFlag;
};

}  // namespace ccv2
