#include "core/constants.h"
#include "core/threadsafe_queue.h"
#include "network/data_sorter.h"
#include "network/socket_receiver2.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <iostream>
#include <memory>

namespace {

QByteArray makeFrames(int frameCount)
{
    QByteArray frames(frameCount * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int f = 0; f < frameCount; ++f) {
        for (int ch = 0; ch < ccv2::kChannelsTotal; ++ch) {
            const qint32 value = 1000 + f + ch;
            char *dst = frames.data() + f * ccv2::kFrameBytes + ch * ccv2::kBytesPerPoint;
            qToLittleEndian<qint32>(value, reinterpret_cast<uchar *>(dst));
        }
    }
    return frames;
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

bool waitForBuffers(const std::shared_ptr<ccv2::RealtimeStreamState> &state, int minSamples, int timeoutMs)
{
    return waitFor([&]() {
        QMutexLocker locker(&state->lock);
        return state->buffers.value(0).size() >= minSamples &&
               state->buffers.value(8).size() >= minSamples;
    }, timeoutMs);
}

bool hasStreamCommand(const QByteArray &command)
{
    return command.startsWith("ctre");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTcpServer controlServer;
    QTcpServer dataServer;
    if (!controlServer.listen(QHostAddress::LocalHost, 0)) {
        std::cerr << "control listen failed" << std::endl;
        return 1;
    }
    if (!dataServer.listen(QHostAddress::LocalHost, 0)) {
        std::cerr << "data listen failed" << std::endl;
        return 2;
    }

    QByteArray controlCommand;
    QByteArray payload = makeFrames(256);
    QTcpSocket *dataClient = nullptr;
    bool dataSent = false;
    bool commandTriggered = false;

    auto trySendData = [&]() {
        if (dataSent || !dataClient || !hasStreamCommand(controlCommand)) {
            return;
        }
        commandTriggered = true;
        dataSent = true;
        dataClient->write(payload.left(333));
        dataClient->write(payload.mid(333, 5000));
        dataClient->write(payload.mid(333 + 5000));
        dataClient->flush();
    };

    QObject::connect(&controlServer, &QTcpServer::newConnection, [&]() {
        QTcpSocket *sock = controlServer.nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::readyRead, [sock, &controlCommand, &trySendData]() {
            controlCommand += sock->readAll();
            trySendData();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    });

    QObject::connect(&dataServer, &QTcpServer::newConnection, [&]() {
        dataClient = dataServer.nextPendingConnection();
        QObject::connect(dataClient, &QTcpSocket::bytesWritten, [&, client = dataClient]() {
            if (client->bytesToWrite() == 0 && dataSent) {
                client->disconnectFromHost();
            }
        });
        QObject::connect(dataClient, &QTcpSocket::disconnected, dataClient, &QObject::deleteLater);
        trySendData();
    });

    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(32);
    auto state = std::make_shared<ccv2::RealtimeStreamState>();
    {
        QMutexLocker locker(&state->lock);
        state->channels = {0, 8};
        state->maxSamples = 512;
        state->buffers[0] = QQueue<double>();
        state->buffers[8] = QQueue<double>();
    }

    std::atomic_bool stopFlag{false};
    ccv2::SocketReceiver2 receiver(QStringLiteral("127.0.0.1"),
                                   static_cast<int>(controlServer.serverPort()),
                                   static_cast<int>(dataServer.serverPort()),
                                   queue,
                                   &stopFlag);
    receiver.setCommand(QStringLiteral("ctre"));

    int receivedBytes = 0;
    QString error;
    QObject::connect(&receiver, &ccv2::SocketReceiver2::bytesReceived, [&](int bytes) {
        receivedBytes += bytes;
    });
    QObject::connect(&receiver, &ccv2::SocketReceiver2::connectionError, [&](const QString &msg) {
        error = msg;
    });

    ccv2::DataSorter sorter(queue, state, &stopFlag);
    sorter.start();
    receiver.start();

    const bool ok = waitForBuffers(state, 128, 5000);

    stopFlag.store(true);
    receiver.wait(2000);
    sorter.wait(2000);

    if (!ok) {
        QMutexLocker locker(&state->lock);
        std::cerr << "socket receiver smoke failed"
                  << " bytes=" << receivedBytes
                  << " ch0=" << state->buffers.value(0).size()
                  << " ch8=" << state->buffers.value(8).size()
                  << " cmd=" << controlCommand.toStdString()
                  << " triggered=" << commandTriggered
                  << " error=" << error.toStdString()
                  << std::endl;
        return 3;
    }

    if (!commandTriggered) {
        std::cerr << "control command did not trigger data: " << controlCommand.toStdString() << std::endl;
        return 4;
    }

    std::cout << "socket_receiver2_smoke ok bytes=" << receivedBytes
              << " cmd=" << controlCommand.toStdString() << std::endl;

    QTcpServer singleServer;
    if (!singleServer.listen(QHostAddress::LocalHost, 0)) {
        std::cerr << "single listen failed" << std::endl;
        return 5;
    }

    QByteArray singleCommand;
    QByteArray singlePayload = makeFrames(128);
    bool singleSent = false;
    QObject::connect(&singleServer, &QTcpServer::newConnection, [&]() {
        QTcpSocket *sock = singleServer.nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::readyRead, [sock, &singleCommand, &singlePayload, &singleSent]() {
            singleCommand += sock->readAll();
            if (!singleSent && hasStreamCommand(singleCommand)) {
                singleSent = true;
                sock->write(singlePayload);
                sock->flush();
            }
        });
        QObject::connect(sock, &QTcpSocket::bytesWritten, [sock, &singleSent]() {
            if (singleSent && sock->bytesToWrite() == 0) {
                sock->disconnectFromHost();
            }
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    });

    auto singleQueue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(32);
    auto singleState = std::make_shared<ccv2::RealtimeStreamState>();
    {
        QMutexLocker locker(&singleState->lock);
        singleState->channels = {0, 8};
        singleState->maxSamples = 512;
        singleState->buffers[0] = QQueue<double>();
        singleState->buffers[8] = QQueue<double>();
    }

    std::atomic_bool singleStopFlag{false};
    ccv2::SocketReceiver2 singleReceiver(QStringLiteral("127.0.0.1"),
                                          static_cast<int>(singleServer.serverPort()),
                                          singleQueue,
                                          &singleStopFlag);
    singleReceiver.setCommand(QStringLiteral("ctre"));
    int singleBytes = 0;
    QString singleError;
    QObject::connect(&singleReceiver, &ccv2::SocketReceiver2::bytesReceived, [&](int bytes) {
        singleBytes += bytes;
    });
    QObject::connect(&singleReceiver, &ccv2::SocketReceiver2::connectionError, [&](const QString &msg) {
        singleError = msg;
    });

    ccv2::DataSorter singleSorter(singleQueue, singleState, &singleStopFlag);
    singleSorter.start();
    singleReceiver.start();

    const bool singleOk = waitForBuffers(singleState, 128, 5000);
    singleStopFlag.store(true);
    singleReceiver.wait(2000);
    singleSorter.wait(2000);

    if (!singleOk) {
        QMutexLocker locker(&singleState->lock);
        std::cerr << "single-port smoke failed"
                  << " bytes=" << singleBytes
                  << " ch0=" << singleState->buffers.value(0).size()
                  << " ch8=" << singleState->buffers.value(8).size()
                  << " cmd=" << singleCommand.toStdString()
                  << " sent=" << singleSent
                  << " error=" << singleError.toStdString()
                  << std::endl;
        return 6;
    }

    std::cout << " socket_receiver2_single_port ok bytes=" << singleBytes
              << " cmd=" << singleCommand.toStdString() << std::endl;

    QTcpServer noDataControlServer;
    QTcpServer noDataDataServer;
    if (!noDataControlServer.listen(QHostAddress::LocalHost, 0)) {
        std::cerr << "no-data control listen failed" << std::endl;
        return 7;
    }
    if (!noDataDataServer.listen(QHostAddress::LocalHost, 0)) {
        std::cerr << "no-data data listen failed" << std::endl;
        return 8;
    }

    QByteArray noDataCommand;
    QObject::connect(&noDataControlServer, &QTcpServer::newConnection, [&]() {
        QTcpSocket *sock = noDataControlServer.nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::readyRead, [sock, &noDataCommand]() {
            noDataCommand += sock->readAll();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    });
    QObject::connect(&noDataDataServer, &QTcpServer::newConnection, [&]() {
        QTcpSocket *sock = noDataDataServer.nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    });

    auto noDataQueue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(32);
    std::atomic_bool noDataStopFlag{false};
    ccv2::SocketReceiver2 noDataReceiver(QStringLiteral("127.0.0.1"),
                                          static_cast<int>(noDataControlServer.serverPort()),
                                          static_cast<int>(noDataDataServer.serverPort()),
                                          noDataQueue,
                                          &noDataStopFlag);
    noDataReceiver.setCommand(QStringLiteral("ctre"));
    QString noDataError;
    bool noDataControlReady = false;
    QObject::connect(&noDataReceiver,
                     &ccv2::SocketReceiver2::controlConnectionEstablished,
                     [&](const QString &) { noDataControlReady = true; });
    QObject::connect(&noDataReceiver, &ccv2::SocketReceiver2::connectionError, [&](const QString &msg) {
        noDataError = msg;
    });
    noDataReceiver.start();

    const bool noDataOk = waitFor([&]() {
        return noDataStopFlag.load() && noDataError.contains(QStringLiteral("No DMA data"));
    }, 5000);
    noDataStopFlag.store(true);
    noDataQueue->wakeAll();
    noDataReceiver.wait(2000);

    if (!noDataOk || !noDataControlReady) {
        std::cerr << "no-data timeout smoke failed"
                  << " cmd=" << noDataCommand.toStdString()
                  << " error=" << noDataError.toStdString()
                  << " stopped=" << noDataStopFlag.load()
                  << " controlReady=" << noDataControlReady
                  << std::endl;
        return 9;
    }

    std::cout << " socket_receiver2_no_data_timeout ok"
              << " cmd=" << noDataCommand.toStdString()
              << " error=" << noDataError.toStdString() << std::endl;
    return 0;
}
