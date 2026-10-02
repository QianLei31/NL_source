#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <QtEndian>

#include <cstdio>
#include <memory>

#include "core/constants.h"
#include "core/threadsafe_queue.h"
#include "io/session_manifest.h"
#include "network/replay_controller.h"
#include "service/session_hub.h"

namespace {

QByteArray makeFrames(int count)
{
    QByteArray data(count * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int frame = 0; frame < count; ++frame) {
        char *frameData = data.data() + frame * ccv2::kFrameBytes;
        for (int channel = 0; channel < ccv2::kChannelsTotal; ++channel) {
            const quint32 raw =
                (static_cast<quint32>(frame) << ccv2::kTimestampShift) |
                static_cast<quint32>((frame + channel) & ccv2::kAdcSampleMask);
            qToLittleEndian<quint32>(
                raw,
                reinterpret_cast<uchar *>(
                    frameData + channel * ccv2::kBytesPerPoint));
        }
    }
    return data;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    constexpr int frameCount = 40;
    constexpr double sampleRate = 400.0;
    const QString root =
        QDir::temp().filePath(QStringLiteral("v6_session_replay_smoke"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    const QByteArray expected = makeFrames(frameCount);
    const QString inputPath = QDir(root).filePath(QStringLiteral("input.bin"));
    QFile input(inputPath);
    if (!input.open(QIODevice::WriteOnly) ||
        input.write(expected) != expected.size()) {
        std::printf("FAIL: cannot create input\n");
        return 1;
    }
    input.close();

    ccv2::SessionHub recordingHub;
    if (!recordingHub.startReplay(inputPath, sampleRate)) {
        std::printf("FAIL: replay open\n");
        return 2;
    }
    if (recordingHub.state() != ccv2::SessionHub::State::ReplayReady) {
        std::printf("FAIL: replay did not remain paused after load\n");
        return 3;
    }
    if (!recordingHub.startRecording(root,
                                     QStringLiteral("recorded"),
                                     4LL * ccv2::kFrameBytes)) {
        std::printf("FAIL: recording start\n");
        return 4;
    }
    QElapsedTimer replayWallClock;
    replayWallClock.start();
    recordingHub.replayTogglePlay();
    QEventLoop replayLoop;
    bool replayFinished = false;
    QObject::connect(&recordingHub,
                     &ccv2::SessionHub::replayFinished,
                     &replayLoop,
                     [&]() {
        replayFinished = true;
        replayLoop.quit();
    });
    QTimer::singleShot(3000, &replayLoop, &QEventLoop::quit);
    replayLoop.exec();
    if (!replayFinished) {
        std::printf("FAIL: replay timeout\n");
        return 5;
    }
    const qint64 replayElapsedMs = replayWallClock.elapsed();
    if (replayElapsedMs < 70 || replayElapsedMs > 350) {
        std::printf("FAIL: 1x timing elapsed=%lld ms\n",
                    static_cast<long long>(replayElapsedMs));
        return 5;
    }
    recordingHub.stop();

    const QString sessionPath = recordingHub.recordingPath();
    ccv2::SessionManifestData manifest;
    QString manifestError;
    if (!ccv2::SessionManifest::read(sessionPath, &manifest, &manifestError)) {
        std::printf("FAIL: manifest read: %s\n", qPrintable(manifestError));
        return 6;
    }
    if (!manifest.complete ||
        manifest.metadata.sampleRate != sampleRate ||
        manifest.parts.size() != 10) {
        std::printf("FAIL: manifest content complete=%d fs=%.1f parts=%lld\n",
                    manifest.complete,
                    manifest.metadata.sampleRate,
                    static_cast<long long>(manifest.parts.size()));
        return 7;
    }

    QByteArray concatenated;
    for (const ccv2::SessionPartInfo &part : manifest.parts) {
        QFile file(QDir(sessionPath).filePath(part.fileName));
        if (!file.open(QIODevice::ReadOnly)) {
            std::printf("FAIL: missing part\n");
            return 8;
        }
        concatenated.append(file.readAll());
    }
    if (concatenated != expected) {
        std::printf("FAIL: recorded bytes differ from source\n");
        return 9;
    }

    ccv2::SessionHub seekHub;
    auto subscriber =
        std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(8);
    seekHub.addSubscriber(subscriber);
    const QString firstPart =
        QDir(sessionPath).filePath(manifest.parts.first().fileName);
    if (!seekHub.startReplay(firstPart, 12345.0)) {
        std::printf("FAIL: split session replay open\n");
        return 10;
    }
    QEventLoop idleLoop;
    QTimer::singleShot(100, &idleLoop, &QEventLoop::quit);
    idleLoop.exec();
    if (subscriber->size() != 0 ||
        seekHub.state() != ccv2::SessionHub::State::ReplayReady ||
        seekHub.sampleRate() != sampleRate) {
        std::printf("FAIL: subscriber changed paused replay state\n");
        return 11;
    }

    seekHub.replaySeekFraction(0.5);
    if (subscriber->size() != 0) {
        std::printf("FAIL: seek did not clear subscriber queue\n");
        return 12;
    }
    seekHub.replayTogglePlay();

    QByteArray firstChunk;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1500 &&
           !subscriber->pop(firstChunk, 20)) {
        QCoreApplication::processEvents();
    }
    if (firstChunk.size() < ccv2::kFrameBytes) {
        std::printf("FAIL: no post-seek data\n");
        return 13;
    }
    const quint32 firstRaw = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(firstChunk.constData()));
    const quint32 timestamp = firstRaw >> ccv2::kTimestampShift;
    if (timestamp != frameCount / 2) {
        std::printf("FAIL: stale pre-seek frame timestamp=%u\n", timestamp);
        return 14;
    }
    seekHub.stop();

    auto blockedQueue =
        std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(1);
    blockedQueue->push(QByteArray(ccv2::kFrameBytes, '\0'));
    ccv2::ReplayController blockedReplay(blockedQueue);
    bool sawBackpressure = false;
    QObject::connect(&blockedReplay,
                     &ccv2::ReplayController::backpressureChanged,
                     &app,
                     [&](bool active) {
        sawBackpressure = sawBackpressure || active;
    });
    if (!blockedReplay.open(inputPath, sampleRate)) {
        std::printf("FAIL: backpressure replay open\n");
        return 15;
    }
    blockedReplay.play();
    QEventLoop blockedLoop;
    QTimer::singleShot(120, &blockedLoop, &QEventLoop::quit);
    blockedLoop.exec();
    if (!sawBackpressure || blockedReplay.currentFrame() != 0) {
        std::printf("FAIL: replay advanced while queue was full\n");
        return 16;
    }
    QByteArray filler;
    blockedQueue->pop(filler, 0);
    QEventLoop resumeLoop;
    QTimer::singleShot(120, &resumeLoop, &QEventLoop::quit);
    resumeLoop.exec();
    if (blockedReplay.currentFrame() <= 0) {
        std::printf("FAIL: replay did not resume after backpressure\n");
        return 17;
    }
    blockedReplay.close();

    const QString malformedDir = QDir(root).filePath(QStringLiteral("malformed"));
    QDir().mkpath(malformedDir);
    QFile malformedBin(QDir(malformedDir).filePath(QStringLiteral("ADC_DATA.bin")));
    QFile malformedManifest(QDir(malformedDir).filePath(QStringLiteral("session.json")));
    if (!malformedBin.open(QIODevice::WriteOnly) ||
        malformedBin.write(expected) != expected.size() ||
        !malformedManifest.open(QIODevice::WriteOnly) ||
        malformedManifest.write("{not valid json") <= 0) {
        std::printf("FAIL: malformed-session fixture\n");
        return 18;
    }
    malformedBin.close();
    malformedManifest.close();
    ccv2::ReplayController malformedReplay(
        std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(4));
    if (malformedReplay.open(malformedBin.fileName(), sampleRate) ||
        malformedReplay.errorString().isEmpty()) {
        std::printf("FAIL: malformed manifest silently fell back to one BIN\n");
        return 19;
    }

    std::printf("PASS: recording, split replay/export guards, seek barrier and backpressure\n");
    return 0;
}
