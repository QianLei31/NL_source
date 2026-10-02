#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

#include "service/dummy_stream_server.h"
#include "service/session_hub.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    constexpr int badPort = 39241;
    constexpr int goodPort = 39242;

    ccv2::SessionHub hub;
    QEventLoop errorLoop;
    bool reachedError = false;
    QObject::connect(&hub,
                     &ccv2::SessionHub::stateChanged,
                     &errorLoop,
                     [&](ccv2::SessionHub::State state) {
        if (state == ccv2::SessionHub::State::Error) {
            reachedError = true;
            errorLoop.quit();
        }
    });
    if (!hub.start(QStringLiteral("127.0.0.1"),
                   badPort,
                   badPort,
                   QStringLiteral("ctre"),
                   20000.0)) {
        std::printf("FAIL: initial start rejected\n");
        return 1;
    }
    QTimer::singleShot(7000, &errorLoop, &QEventLoop::quit);
    errorLoop.exec();
    if (!reachedError || hub.isRunning()) {
        std::printf("FAIL: failed connection did not settle in Error\n");
        return 2;
    }

    ccv2::DummyStreamServer dummy;
    QString error;
    if (!dummy.start(goodPort, goodPort, &error)) {
        std::printf("FAIL: dummy start: %s\n", qPrintable(error));
        return 3;
    }

    QEventLoop connectedLoop;
    bool connected = false;
    QObject::connect(&hub,
                     &ccv2::SessionHub::connectionStateChanged,
                     &connectedLoop,
                     [&](bool value) {
        if (value) {
            connected = true;
            connectedLoop.quit();
        }
    });
    if (!hub.start(QStringLiteral("127.0.0.1"),
                   goodPort,
                   goodPort,
                   QStringLiteral("ctre"),
                   20000.0)) {
        std::printf("FAIL: one-click retry rejected\n");
        return 4;
    }
    QTimer::singleShot(5000, &connectedLoop, &QEventLoop::quit);
    connectedLoop.exec();
    if (!connected || hub.state() != ccv2::SessionHub::State::Live) {
        std::printf("FAIL: one-click retry did not connect\n");
        return 5;
    }

    bool disconnectedSignal = false;
    QObject::connect(&hub,
                     &ccv2::SessionHub::connectionStateChanged,
                     &app,
                     [&](bool value) {
        if (!value) disconnectedSignal = true;
    });
    hub.stop();
    if (!disconnectedSignal ||
        hub.state() != ccv2::SessionHub::State::Idle ||
        hub.isRunning()) {
        std::printf("FAIL: stop state/signals inconsistent\n");
        return 6;
    }

    std::printf("PASS: error state, one-click retry and clean stop\n");
    return 0;
}
