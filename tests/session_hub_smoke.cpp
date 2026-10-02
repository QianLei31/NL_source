// Integration smoke test for the V6 SessionHub:
//   * recording captures a byte-clean stream (multiple of the 1024-byte frame),
//   * a stalled display subscriber (never drained) does NOT slow recording,
//     proving recording and display are isolated consumers.

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <memory>

#include "core/constants.h"
#include "core/threadsafe_queue.h"
#include "service/dummy_stream_server.h"
#include "service/session_hub.h"

using namespace ccv2;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const int port = 39137;
    DummyStreamServer dummy;
    QString err;
    if (!dummy.start(port, port, &err)) {
        std::printf("FAIL: dummy server start: %s\n", qPrintable(err));
        return 2;
    }

    SessionHub hub;
    qint64 hubDropped = 0;
    QObject::connect(&hub, &SessionHub::framesDropped, &app, [&](qint64 f) { hubDropped += f; });

    const QString baseDir = QDir::tempPath() + QStringLiteral("/v6hubtest");
    QDir(baseDir).removeRecursively();
    QDir().mkpath(baseDir);

    // Deliberately tiny, never-drained subscriber: the worst-case slow display.
    auto stalled = std::make_shared<ThreadSafeQueue<QByteArray>>(32);

    hub.startRecording(baseDir, QStringLiteral("sess"), 0);
    hub.start(QStringLiteral("127.0.0.1"), port, port, QStringLiteral("ctre"));

    int rc = 1;
    qint64 subscriberAttachMs = -1;
    // Warm up past connect latency, then measure two equal windows:
    // window 1 = recording alone; window 2 = recording + stalled subscriber.
    QTimer::singleShot(500, &app, [&]() {
        const qint64 base1 = hub.recordedBytes();
        QTimer::singleShot(1200, &app, [&, base1]() {
            const qint64 w1 = hub.recordedBytes() - base1;
            QElapsedTimer attachTimer;
            attachTimer.start();
            hub.addSubscriber(stalled);  // attach the stalled display consumer
            subscriberAttachMs = attachTimer.elapsed();
            const qint64 base2 = hub.recordedBytes();
            QTimer::singleShot(1200, &app, [&, w1, base2]() {
                const qint64 w2 = hub.recordedBytes() - base2;
                hub.stopRecording();
                hub.stop();

                const QString file = baseDir + QStringLiteral("/sess/ADC_DATA.bin");
                const qint64 fsz = QFileInfo(file).size();
                const bool okNonEmpty = fsz > 0;
                const bool okFrameAligned = (fsz % kFrameBytes) == 0;
                const double rate1 = w1 / 1.2;
                const double rate2 = w2 / 1.2;
                const double deltaPct = rate1 > 0 ? 100.0 * std::abs(rate2 - rate1) / rate1 : 999.0;
                const bool okRate = rate1 > 0 && deltaPct < 20.0;
                const bool okAttachLatency = subscriberAttachMs >= 0 && subscriberAttachMs < 200;

                std::printf("recorded file: %lld bytes (frame=%d)\n", (long long)fsz, kFrameBytes);
                std::printf("frame-aligned (no torn frames): %s\n", okFrameAligned ? "YES" : "NO");
                std::printf("hub frames dropped (receiver): %lld\n", (long long)hubDropped);
                std::printf("record rate window1 (alone):       %.0f B/s\n", rate1);
                std::printf("record rate window2 (+stalled sub): %.0f B/s\n", rate2);
                std::printf("subscriber impact on recording: %s (delta %.1f%%)\n",
                            okRate ? "negligible" : "SIGNIFICANT", deltaPct);
                std::printf("subscriber attach latency: %lld ms (%s)\n",
                            (long long)subscriberAttachMs,
                            okAttachLatency ? "responsive" : "TOO SLOW");

                rc = (okNonEmpty && okFrameAligned && okRate && okAttachLatency) ? 0 : 1;
                std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
                app.quit();
            });
        });
    });

    app.exec();
    return rc;
}
