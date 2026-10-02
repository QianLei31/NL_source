#include "network/socket_receiver2.h"

#include <QNetworkProxy>
#include <QTcpSocket>
#include <QThread>
#include <QElapsedTimer>

#include "core/constants.h"
#include "core/channel_routing.h"

namespace ccv2 {

namespace {
constexpr int kReadPollMs = 250;
constexpr qint64 kFirstDataTimeoutMs = 3000;
constexpr qint64 kIdleDataTimeoutMs = 5000;
constexpr int kConnectPollMs = 100;
constexpr int kFramesPerQueueBatch = 64;
// Ingress batches are dropped whole under backpressure before they are ever
// numbered. A four-frame-aligned batch keeps the TDM phase assignment intact
// across such drops; consumer-side drop accounting handles everything after
// numbering. Do not make this odd.
static_assert(kFramesPerQueueBatch % kTdmPhaseCount == 0,
              "TDM phase relies on four-frame-aligned ingress batches");

void requestStop(std::atomic_bool *stopFlag,
                 const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue)
{
    if (stopFlag) {
        stopFlag->store(true);
    }
    if (queue) {
        queue->wakeAll();
    }
}

bool waitForConnectedInterruptible(QTcpSocket &socket, int timeoutMs, std::atomic_bool *stopFlag)
{
    QElapsedTimer timer;
    timer.start();

    while (socket.state() != QAbstractSocket::ConnectedState && timer.elapsed() < timeoutMs) {
        if (stopFlag && stopFlag->load()) {
            socket.abort();
            return false;
        }

        const int remainingMs = timeoutMs - static_cast<int>(timer.elapsed());
        const int waitMs = qMax(1, qMin(kConnectPollMs, remainingMs));
        if (socket.waitForConnected(waitMs)) {
            return true;
        }

        if (socket.state() == QAbstractSocket::UnconnectedState &&
            socket.error() != QAbstractSocket::SocketTimeoutError) {
            return false;
        }
    }

    return socket.state() == QAbstractSocket::ConnectedState;
}

qint64 enqueueCompleteFrames(const QByteArray &chunk,
                             QByteArray &leftover,
                             const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue,
                             bool flush = false)
{
    if (!queue) {
        return 0;
    }

    leftover.append(chunk);
    const int completeFrames = leftover.size() / kFrameBytes;
    const int queuedFrames = flush
                                 ? completeFrames
                                 : (completeFrames / kFramesPerQueueBatch) *
                                       kFramesPerQueueBatch;
    const int completeBytes = queuedFrames * kFrameBytes;
    if (completeBytes <= 0) {
        return 0;
    }

    const QByteArray frames = leftover.left(completeBytes);
    leftover.remove(0, completeBytes);

    qint64 droppedFrames = 0;
    const int batchBytes = kFramesPerQueueBatch * kFrameBytes;
    for (int offset = 0; offset < frames.size(); offset += batchBytes) {
        const QByteArray batch =
            frames.mid(offset, qMin(batchBytes, frames.size() - offset));
        QByteArray dropped;
        const bool queuedWithoutDrop = queue->push(batch, true, &dropped);
        if (!queuedWithoutDrop && !dropped.isEmpty()) {
            droppedFrames += dropped.size() / kFrameBytes;
        }
    }
    return droppedFrames;
}
}  // namespace

// Legacy single-port constructor (backward compat)
SocketReceiver2::SocketReceiver2(const QString &host, int port,
                                 std::shared_ptr<ThreadSafeQueue<QByteArray>> outputQueue,
                                 std::atomic_bool *stopFlag, QObject *parent)
    : QThread(parent),
      m_host(host),
      m_controlPort(port),
      m_dataPort(port),
      m_dualMode(false),
      m_outputQueue(std::move(outputQueue)),
      m_stopFlag(stopFlag) {}

// New dual-port constructor
SocketReceiver2::SocketReceiver2(const QString &host, int controlPort, int dataPort,
                                 std::shared_ptr<ThreadSafeQueue<QByteArray>> outputQueue,
                                 std::atomic_bool *stopFlag, QObject *parent)
    : QThread(parent),
      m_host(host),
      m_controlPort(controlPort),
      m_dataPort(dataPort),
      m_dualMode(true),
      m_outputQueue(std::move(outputQueue)),
      m_stopFlag(stopFlag) {}

void SocketReceiver2::run() {
    if (m_dualMode) {
        // === DUAL PORT MODE ===
        // Step 1: Connect control socket (port 7) first.
        QTcpSocket controlSocket;
        controlSocket.setProxy(QNetworkProxy::NoProxy);
        controlSocket.connectToHost(m_host, m_controlPort);
        if (!waitForConnectedInterruptible(controlSocket, 5000, m_stopFlag)) {
            if (!m_stopFlag || !m_stopFlag->load()) {
                emit connectionError(QStringLiteral("Failed to connect control port %1:%2: %3")
                                         .arg(m_host).arg(m_controlPort).arg(controlSocket.errorString()));
            }
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }

        const QByteArray cmdBytes = m_command.trimmed().toUtf8();
        if (cmdBytes.isEmpty()) {
            emit connectionError(QStringLiteral("Empty stream command"));
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }

        // Step 2: Send ctre on the control socket before opening the data socket.
        // The hardware path expects port 7 to arm the stream, then port 5001 to
        // provide continuous ADC data.
        controlSocket.write(cmdBytes);
        if (!controlSocket.waitForBytesWritten(1000)) {
            emit connectionError(QStringLiteral("Failed to send stream command to control port %1:%2: %3")
                                     .arg(m_host).arg(m_controlPort).arg(controlSocket.errorString()));
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }
        emit controlConnectionEstablished(
            QStringLiteral("Control available: %1:%2").arg(m_host).arg(m_controlPort));

        QTcpSocket dataSocket;
        dataSocket.setProxy(QNetworkProxy::NoProxy);
        dataSocket.setReadBufferSize(4 * 1024 * 1024);
        dataSocket.connectToHost(m_host, m_dataPort);
        const bool dataConnected = waitForConnectedInterruptible(dataSocket, 5000, m_stopFlag);
        if (!dataConnected) {
            if (!m_stopFlag || !m_stopFlag->load()) {
                emit connectionError(QStringLiteral("Failed to connect data port %1:%2: %3")
                                         .arg(m_host).arg(m_dataPort).arg(dataSocket.errorString()));
            }
            controlSocket.write("stop");
            controlSocket.waitForBytesWritten(500);
            controlSocket.disconnectFromHost();
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }

        emit connectionEstablished(QStringLiteral("Connected: ctrl=%1:%2, data=%1:%3")
                                       .arg(m_host).arg(m_controlPort).arg(m_dataPort));

        // Step 3: Read data continuously from data socket (5001). If PL/DMA is
        // not producing data, fail fast so the UI does not sit in a fake running state.
        QElapsedTimer firstDataTimer;
        QElapsedTimer idleTimer;
        firstDataTimer.start();
        idleTimer.start();
        bool receivedAnyData = false;
        QByteArray frameLeftover;
        while (!m_stopFlag->load()) {
            if (!dataSocket.waitForReadyRead(kReadPollMs)) {
                if (dataSocket.state() != QAbstractSocket::ConnectedState) {
                    emit connectionError(QStringLiteral("Data socket disconnected"));
                    break;
                }
                if (!receivedAnyData && firstDataTimer.elapsed() >= kFirstDataTimeoutMs) {
                    emit connectionError(QStringLiteral("No DMA data received within %1 ms")
                                             .arg(kFirstDataTimeoutMs));
                    requestStop(m_stopFlag, m_outputQueue);
                    break;
                }
                if (receivedAnyData && idleTimer.elapsed() >= kIdleDataTimeoutMs) {
                    emit connectionError(QStringLiteral("DMA data stalled for %1 ms")
                                             .arg(kIdleDataTimeoutMs));
                    requestStop(m_stopFlag, m_outputQueue);
                    break;
                }
                continue;
            }

            QByteArray chunk = dataSocket.readAll();
            if (!chunk.isEmpty()) {
                receivedAnyData = true;
                idleTimer.restart();
                const qint64 droppedFrames =
                    enqueueCompleteFrames(chunk, frameLeftover, m_outputQueue);
                if (droppedFrames > 0) {
                    emit framesDropped(droppedFrames);
                }
                emit bytesReceived(chunk.size());
            }
        }

        const qint64 tailDrops =
            enqueueCompleteFrames(QByteArray(), frameLeftover, m_outputQueue, true);
        if (tailDrops > 0) emit framesDropped(tailDrops);

        // Cleanup: send stop to control port
        if (controlSocket.state() == QAbstractSocket::ConnectedState) {
            controlSocket.write("stop");
            controlSocket.waitForBytesWritten(500);
            controlSocket.disconnectFromHost();
            controlSocket.waitForDisconnected(500);
        }
        dataSocket.disconnectFromHost();
        dataSocket.waitForDisconnected(500);

    } else {
        // === LEGACY SINGLE PORT MODE ===
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.setReadBufferSize(4 * 1024 * 1024);
        socket.connectToHost(m_host, m_controlPort);
        if (!waitForConnectedInterruptible(socket, 5000, m_stopFlag)) {
            if (!m_stopFlag || !m_stopFlag->load()) {
                emit connectionError(QStringLiteral("Failed to connect %1:%2: %3")
                                         .arg(m_host).arg(m_controlPort).arg(socket.errorString()));
            }
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }

        const QByteArray cmdBytes = m_command.trimmed().toUtf8();
        if (cmdBytes.isEmpty()) {
            emit connectionError(QStringLiteral("Empty stream command"));
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }
        socket.write(cmdBytes);
        if (!socket.waitForBytesWritten(1000)) {
            emit connectionError(QStringLiteral("Failed to send stream command to %1:%2: %3")
                                     .arg(m_host).arg(m_controlPort).arg(socket.errorString()));
            requestStop(m_stopFlag, m_outputQueue);
            return;
        }
        emit controlConnectionEstablished(
            QStringLiteral("Control available: %1:%2").arg(m_host).arg(m_controlPort));
        emit connectionEstablished(QStringLiteral("Connected (single): %1:%2").arg(m_host).arg(m_controlPort));

        QElapsedTimer firstDataTimer;
        QElapsedTimer idleTimer;
        firstDataTimer.start();
        idleTimer.start();
        bool receivedAnyData = false;
        QByteArray frameLeftover;
        while (!m_stopFlag->load()) {
            if (!socket.waitForReadyRead(kReadPollMs)) {
                if (socket.state() != QAbstractSocket::ConnectedState) {
                    emit connectionError(QStringLiteral("Socket disconnected"));
                    break;
                }
                if (!receivedAnyData && firstDataTimer.elapsed() >= kFirstDataTimeoutMs) {
                    emit connectionError(QStringLiteral("No DMA data received within %1 ms")
                                             .arg(kFirstDataTimeoutMs));
                    requestStop(m_stopFlag, m_outputQueue);
                    break;
                }
                if (receivedAnyData && idleTimer.elapsed() >= kIdleDataTimeoutMs) {
                    emit connectionError(QStringLiteral("DMA data stalled for %1 ms")
                                             .arg(kIdleDataTimeoutMs));
                    requestStop(m_stopFlag, m_outputQueue);
                    break;
                }
                continue;
            }

            QByteArray chunk = socket.readAll();
            if (!chunk.isEmpty()) {
                receivedAnyData = true;
                idleTimer.restart();
                const qint64 droppedFrames =
                    enqueueCompleteFrames(chunk, frameLeftover, m_outputQueue);
                if (droppedFrames > 0) {
                    emit framesDropped(droppedFrames);
                }
                emit bytesReceived(chunk.size());
            }
        }

        const qint64 tailDrops =
            enqueueCompleteFrames(QByteArray(), frameLeftover, m_outputQueue, true);
        if (tailDrops > 0) emit framesDropped(tailDrops);

        socket.disconnectFromHost();
        socket.waitForDisconnected(500);
    }

    // NOTE: Do NOT set m_stopFlag here — let the sorter drain the queue first.
    // The caller (page) is responsible for setting stopFlag after sorter finishes.
}

}  // namespace ccv2
