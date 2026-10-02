#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cstdio>
#include <functional>
#include <memory>

#include "core/constants.h"
#include "io/recording_worker.h"
#include "io/session_recorder.h"
#include "network/replay_controller.h"

namespace {
using namespace ccv2;
#define REQUIRE(condition, label) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); return 1; } } while (false)

bool waitUntil(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return done();
}

bool invalid(qint64 frame) { return (frame >= 2 && frame < 5) || (frame >= 7 && frame < 9); }
QByteArray frames(int begin, int count) {
    QByteArray bytes(count * kFrameBytes, '\0');
    for (int f = 0; f < count; ++f) {
        if (invalid(begin + f)) continue;
        const quint32 timestamp = 500 + begin + f;
        for (int c = 0; c < kChannelsTotal; ++c)
            qToLittleEndian<quint32>((timestamp << kTimestampShift) | 1234u,
                bytes.data() + f * kFrameBytes + c * kBytesPerPoint);
    }
    return bytes;
}
StreamBlockInfo block(int begin, int count) {
    StreamBlockInfo info;
    info.epoch = 9;
    for (int f = 0; f < count; ++f) {
        info.frameIndices.push_back(120 + begin + f);
        info.frameValid.push_back(!invalid(begin + f));
    }
    return info;
}
bool saveJson(const QString &folder, const QJsonObject &root) {
    QFile file(QDir(folder).filePath(QStringLiteral("session.json")));
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(root).toJson()) > 0;
}
QJsonObject readJson(const QString &folder) {
    QFile file(QDir(folder).filePath(QStringLiteral("session.json")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    REQUIRE(dir.isValid(), "temporary directory");
    SessionMetadata metadata;
    metadata.sampleRate = 4000;
    metadata.frameOrigin = 120;
    metadata.neuralAnalysis = {{QStringLiteral("revision"), 1}};
    SessionRecorder recorder;
    const QString folder = recorder.start(dir.path(), QStringLiteral("masked"), 4 * kFrameBytes, metadata);
    REQUIRE(!folder.isEmpty(), "start recorder");
    // The first invalid interval crosses both this write boundary and a part boundary.
    REQUIRE(recorder.write(frames(0, 3), block(0, 3)), "write first block");
    REQUIRE(recorder.write(frames(3, 9), block(3, 9)), "write split second block");
    recorder.setAnalysisMetadata({{QStringLiteral("revision"), 2}});
    REQUIRE(recorder.stop(true), "stop recorder");
    SessionManifestData manifest;
    QString error;
    REQUIRE(SessionManifest::read(folder, &manifest, &error), "read new manifest");
    REQUIRE(manifest.complete && manifest.parts.size() == 3, "complete is distinct from validity");
    REQUIRE(manifest.frameValidityKnown && manifest.invalidFrameRanges.size() == 2, "exact invalid range count");
    SessionManifestData invalidWrite = manifest;
    invalidWrite.invalidFrameRanges = {{11, 2}};
    REQUIRE(!SessionManifest::write(folder, invalidWrite, &error), "writer rejects invalid interval");
    REQUIRE(manifest.invalidFrameRanges[0].startFrame == 2 && manifest.invalidFrameRanges[0].frameCount == 3,
            "coalesce invalid range across writes and splits");
    REQUIRE(manifest.invalidFrameRanges[1].startFrame == 7 && manifest.invalidFrameRanges[1].frameCount == 2,
            "second range coordinates");
    REQUIRE(manifest.metadata.neuralAnalysis.value(QStringLiteral("revision")).toInt() == 2,
            "final analysis metadata persisted");
    QByteArray raw;
    for (const auto &part : manifest.parts) {
        QFile file(QDir(folder).filePath(part.fileName));
        REQUIRE(file.open(QIODevice::ReadOnly), "open raw part");
        raw += file.readAll();
    }
    REQUIRE(raw == frames(0, 12), "raw BIN byte compatibility");
    const QJsonObject good = readJson(folder);
    REQUIRE(good.value(QStringLiteral("schema_version")).toInt() == 1, "preserve schema one");

    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    ReplayController replay(queue);
    REQUIRE(replay.open(folder, 1), "open split recording");
    REQUIRE(replay.frameValidityKnown() && !replay.integrityComplete() && !replay.integrityUnknown(),
            "validity does not claim complete integrity");
    REQUIRE(replay.metadata().neuralAnalysis == manifest.metadata.neuralAnalysis, "replay metadata exposed");
    replay.play();
    REQUIRE(waitUntil([&] { return replay.currentFrame() == 12; }), "finish masked replay");
    QByteArray replayBytes;
    QVector<bool> replayMask;
    QVector<qint64> replayIndices;
    while (queue->size()) {
        QByteArray chunk;
        StreamBlockInfo info;
        REQUIRE(queue->pop(chunk, 0, nullptr, &info), "pop replay block");
        REQUIRE(info.frameValid.size() == chunk.size() / kFrameBytes, "mask matches block");
        replayBytes += chunk;
        replayMask += info.frameValid;
        replayIndices += info.frameIndices;
    }
    REQUIRE(replayBytes == raw && replayMask.size() == 12, "replay all split bytes");
    for (int i = 0; i < 12; ++i) {
        REQUIRE(replayMask[i] == !invalid(i), "exact replay validity");
        REQUIRE(replayIndices[i] == 120 + i, "placeholder timestamps preserve source coordinates");
    }
    replay.seekFrame(3); // Starts inside the invalid interval, across split 0/1.
    REQUIRE(waitUntil([&] { return !replay.isSeeking() && replay.currentFrame() == 3; }), "seek inside invalid range");
    replay.play();
    REQUIRE(waitUntil([&] { return replay.currentFrame() == 12; }), "finish post-seek replay");
    int cursor = 3;
    while (queue->size()) {
        QByteArray chunk;
        StreamBlockInfo info;
        REQUIRE(queue->pop(chunk, 0, nullptr, &info), "pop post-seek block");
        for (int f = 0; f < info.frameValid.size(); ++f, ++cursor) {
            REQUIRE(info.frameValid[f] == !invalid(cursor), "seek mask uses file coordinates");
            REQUIRE(info.frameIndices[f] == 120 + cursor, "seek timeline matches continuous replay");
        }
    }
    REQUIRE(cursor == 12, "post-seek mask complete");
    replay.close();

    // Schema-one sessions before validity metadata remain readable, even when
    // their complete flag incorrectly ignored ingress drops. Analysis is blocked.
    QJsonObject legacy = good;
    legacy.remove(QStringLiteral("frame_validity"));
    legacy.remove(QStringLiteral("neural_analysis"));
    legacy.insert(QStringLiteral("ingress_dropped_frames"), QStringLiteral("5"));
    REQUIRE(saveJson(folder, legacy), "write legacy fixture");
    REQUIRE(replay.open(folder, 1), "legacy session remains readable");
    REQUIRE(!replay.frameValidityKnown() && replay.integrityUnknown() && !replay.integrityComplete() &&
            !replay.warning().isEmpty(), "legacy drop integrity explicitly unknown");
    replay.play();
    REQUIRE(waitUntil([&] { return replay.currentFrame() == 12; }), "legacy replay remains readable");
    while (queue->size()) {
        QByteArray chunk;
        StreamBlockInfo info;
        REQUIRE(queue->pop(chunk, 0, nullptr, &info), "pop legacy block");
        REQUIRE(info.integrityUnknown && info.frameValid.isEmpty(), "legacy unknown reaches neural consumer");
    }
    replay.close();

    const auto rejectRanges = [&](const QJsonArray &ranges) {
        QJsonObject malformed = good;
        QJsonObject validity = malformed.value(QStringLiteral("frame_validity")).toObject();
        validity.insert(QStringLiteral("invalid_ranges"), ranges);
        malformed.insert(QStringLiteral("frame_validity"), validity);
        return saveJson(folder, malformed) && !SessionManifest::read(folder, &manifest, &error);
    };
    const auto range = [](QJsonValue start, QJsonValue count) {
        return QJsonObject{{QStringLiteral("start_frame"), start}, {QStringLiteral("frame_count"), count}};
    };
    REQUIRE(rejectRanges({range(-1, 1)}), "reject negative start");
    REQUIRE(rejectRanges({range(1, 0)}), "reject empty interval");
    REQUIRE(rejectRanges({range(11, 2)}), "reject out-of-bounds interval");
    REQUIRE(rejectRanges({range(2, 3), range(4, 2)}), "reject overlap");
    REQUIRE(rejectRanges({range(7, 1), range(2, 1)}), "reject unsorted intervals");
    REQUIRE(rejectRanges({range(1.5, 1)}), "reject fractional coordinate");
    REQUIRE(rejectRanges({range(QStringLiteral("9223372036854775808"), 1)}), "reject integer overflow");
    REQUIRE(rejectRanges({range(1, QStringLiteral("nope"))}), "reject malformed number");
    QJsonObject versionBad = good;
    QJsonObject validity = versionBad.value(QStringLiteral("frame_validity")).toObject();
    validity.insert(QStringLiteral("version"), 2);
    versionBad.insert(QStringLiteral("frame_validity"), validity);
    REQUIRE(saveJson(folder, versionBad) && !SessionManifest::read(folder, &manifest, &error),
            "reject unsupported validity version");
    QJsonObject badTotal = good;
    badTotal.insert(QStringLiteral("total_frames"), 12.5);
    REQUIRE(saveJson(folder, badTotal) && !SessionManifest::read(folder, &manifest, &error),
            "reject fractional validity coordinate extent");
    REQUIRE(saveJson(folder, good), "restore valid manifest");

    // Active/incomplete sessions with shifted part sizes cannot reuse the mask.
    QJsonObject incomplete = good;
    incomplete.insert(QStringLiteral("complete"), false);
    REQUIRE(saveJson(folder, incomplete), "incomplete fixture");
    QFile firstPart(QDir(folder).filePath(QStringLiteral("ADC_DATA_0000.bin")));
    REQUIRE(firstPart.open(QIODevice::ReadWrite) && firstPart.resize(3 * kFrameBytes), "truncate first part");
    firstPart.close();
    SessionInput resolved;
    REQUIRE(SessionManifest::resolveInput(folder, 1, &resolved, &error), "truncated incomplete session readable");
    REQUIRE(resolved.integrityUnknown && !resolved.frameValidityKnown && resolved.invalidFrameRanges.isEmpty(),
            "shifted part offsets fail closed");

    RecordingWorker worker(dir.path(), QStringLiteral("worker"), 4 * kFrameBytes, metadata);
    REQUIRE(worker.startAndWait(), "start recording worker");
    REQUIRE(worker.enqueue(frames(0, 12), block(0, 12)), "worker accepts annotated block");
    worker.setAnalysisMetadata({{QStringLiteral("revision"), 3}});
    worker.requestStop(true, QStringLiteral("test"), 0);
    REQUIRE(worker.wait(3000), "worker drains and closes");
    REQUIRE(SessionManifest::read(worker.recordingPath(), &manifest, &error), "read worker manifest");
    REQUIRE(manifest.frameValidityKnown && manifest.invalidFrameRanges.size() == 2 &&
            manifest.metadata.frameOrigin == 120 &&
            manifest.metadata.neuralAnalysis.value(QStringLiteral("revision")).toInt() == 3,
            "worker preserves final metadata and validity");
    SessionRecorder clean;
    const QString cleanFolder = clean.start(dir.path(), QStringLiteral("clean"), 0, metadata);
    StreamBlockInfo cleanInfo;
    cleanInfo.frameValid = {true, true};
    REQUIRE(!cleanFolder.isEmpty() && clean.write(frames(0, 2), cleanInfo) && clean.stop(),
            "record explicitly valid frames");
    REQUIRE(replay.open(cleanFolder, 1) && replay.frameValidityKnown() && replay.integrityComplete(),
            "empty invalid ranges still persist known validity");
    replay.close();

    SessionRecorder malformedMask;
    const QString maskFolder = malformedMask.start(dir.path(), QStringLiteral("wrong-mask"), 0, metadata);
    StreamBlockInfo wrongInfo;
    wrongInfo.frameValid = {false};
    REQUIRE(!maskFolder.isEmpty() && !malformedMask.write(frames(0, 2), wrongInfo) &&
            malformedMask.totalBytes() == 0, "reject wrong-length mask before bytes are written");
    REQUIRE(malformedMask.stop(false), "close rejected-mask recorder");
    std::puts("PASS: recording validity ranges, raw compatibility, split/seek masks, legacy safety and validation");
    return 0;
}
