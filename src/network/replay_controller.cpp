#include "network/replay_controller.h"

#include <algorithm>
#include <QPointer>
#include <QtConcurrent>
#include <QtEndian>

#include "core/constants.h"
#include "core/channel_routing.h"

namespace ccv2 {
namespace {
constexpr int kTickMs = 25;
constexpr qint64 kIndexStride = 4096;
bool frameIsInvalid(const QVector<SessionFrameRange> &ranges, qint64 frame) {
    const auto it = std::upper_bound(ranges.cbegin(), ranges.cend(), frame,
        [](qint64 value, const SessionFrameRange &range) { return value < range.startFrame; });
    if (it == ranges.cbegin()) return false;
    const auto &range = *(it - 1);
    return frame - range.startFrame < range.frameCount;
}
qint64 advanceFileFrame(FrameTimestampReconciler &timeline, const char *frame, bool invalid) {
    const quint32 timestamp = qFromLittleEndian<quint32>(frame) >> kTimestampShift;
    for (int ch=1; !invalid && ch<kChannelsTotal; ++ch)
        invalid = (qFromLittleEndian<quint32>(frame + ch*kBytesPerPoint) >> kTimestampShift) != timestamp;
    if (invalid) {
        const qint64 index = timeline.nextIndex();
        timeline.skipFrames(1); // Synthetic zeros must not reset timestamp phase.
        return index;
    }
    return timeline.advance((qFromLittleEndian<quint32>(frame) >> kTimestampShift) & 0xFFFFFu);
}
}

ReplayController::ReplayController(std::shared_ptr<ThreadSafeQueue<QByteArray>> queue, QObject *parent)
    : QObject(parent), m_queue(std::move(queue)), m_frameBytes(kFrameBytes) {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &ReplayController::tick);
}

ReplayController::~ReplayController() { close(); }

bool ReplayController::open(const QString &binFile, double sampleRate) {
    close();
    if (!loadParts(binFile, sampleRate) || !openPart(0)) return false;
    m_sourcePath = binFile;
    emit positionChanged(0, m_totalFrames);
    return true;
}

void ReplayController::cancelSeek() {
    ++m_seekGeneration;
    if (m_seekCancel) m_seekCancel->store(true);
    if (m_seekFuture.isRunning()) m_seekFuture.waitForFinished();
    m_seeking = false;
    m_playAfterSeek = false;
}

void ReplayController::close() {
    cancelSeek();
    pause();
    m_file.close();
    m_parts.clear();
    m_checkpoints.clear();
    m_partIndex = -1;
    m_sourcePath.clear();
    m_metadata = {};
    m_sourceProvenance = {};
    m_frameValidityKnown = false;
    m_integrityComplete = false;
    m_integrityUnknown = false;
    m_invalidFrameRanges.clear();
    m_warning.clear();
    m_error.clear();
    m_totalFrames = m_curFrame = 0;
    m_frameAccum = 0.0;
    m_pendingChunk.clear();
    m_pendingInfo = {};
    m_pendingFrames = 0;
    if (m_backpressured) {
        m_backpressured = false;
        emit backpressureChanged(false);
    }
}

void ReplayController::play() {
    if (m_seeking) { m_playAfterSeek = true; return; }
    if (m_parts.isEmpty() || m_timer.isActive()) return;
    if (m_curFrame >= m_totalFrames) {
        m_playAfterSeek = true;
        seekFrame(0);
        return;
    }
    if (!m_file.isOpen() || !m_error.isEmpty()) return;
    m_clock.restart();
    m_timer.start();
    emit playingChanged(true);
}

void ReplayController::pause() {
    m_playAfterSeek = false;
    if (!m_timer.isActive()) return;
    m_timer.stop();
    emit playingChanged(false);
}

void ReplayController::togglePlay() {
    if (m_timer.isActive() || (m_seeking && m_playAfterSeek)) pause();
    else play();
}

void ReplayController::applySeek(qint64 frame, const FrameTimestampReconciler &timeline) {
    int index = m_parts.size() - 1;
    for (int i = 0; i < m_parts.size(); ++i) {
        if (frame < m_parts[i].startFrame + m_parts[i].frames) { index = i; break; }
    }
    if (!openPart(index)) { failPlayback(m_error); return; }
    const qint64 local = qBound<qint64>(0, frame - m_parts[index].startFrame, m_parts[index].frames);
    if (!m_file.seek(local * m_frameBytes)) {
        failPlayback(QStringLiteral("回放定位失败: %1").arg(m_file.errorString()));
        return;
    }
    const bool resume = m_playAfterSeek;
    m_seeking = false;
    m_playAfterSeek = false;
    m_curFrame = frame;
    m_readerTimeline = timeline;
    m_pendingChunk.clear();
    m_pendingInfo = {};
    m_pendingFrames = 0;
    m_frameAccum = 0.0;
    m_queue->clear();
    emit timelinePositioned(timeline.nextIndex());
    emit positionChanged(m_curFrame, m_totalFrames);
    if (resume && frame < m_totalFrames) play();
}

void ReplayController::seekFrame(qint64 frame) {
    if (m_parts.isEmpty()) return;
    const bool resume = m_timer.isActive() || m_playAfterSeek;
    pause();
    cancelSeek();
    m_playAfterSeek = resume;
    m_error.clear();
    frame = qBound<qint64>(0, frame, m_totalFrames);
    m_queue->clear();
    m_pendingChunk.clear();
    m_pendingInfo = {};
    m_pendingFrames = 0;
    if (m_backpressured) { m_backpressured = false; emit backpressureChanged(false); }
    emit seekStarted(frame);

    auto checkpoint = m_checkpoints.upperBound(frame);
    --checkpoint;
    if (checkpoint.key() == frame) { applySeek(frame, checkpoint.value()); return; }

    m_seeking = true;
    m_seekCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_seekCancel;
    const auto parts = m_parts;
    const auto invalidRanges = m_invalidFrameRanges;
    const qint64 beginFrame = checkpoint.key();
    const auto beginTimeline = checkpoint.value();
    const quint64 generation = m_seekGeneration;
    QPointer<ReplayController> self(this);
    // Scan only the not-yet-indexed prefix off-thread, retaining sparse
    // checkpoints instead of loading the whole recording into memory.
    m_seekFuture = QtConcurrent::run([self, cancel, parts, invalidRanges, beginFrame, beginTimeline, frame, generation]() {
        auto timeline = beginTimeline;
        qint64 cursor = beginFrame;
        QMap<qint64, FrameTimestampReconciler> added;
        QString error;
        for (const auto &part : parts) {
            if (cursor >= frame || cancel->load()) break;
            if (cursor >= part.startFrame + part.frames) continue;
            QFile input(part.path);
            if (!input.open(QIODevice::ReadOnly) ||
                !input.seek((cursor - part.startFrame) * kFrameBytes)) {
                error = QStringLiteral("无法索引回放分片: %1").arg(part.path);
                break;
            }
            while (cursor < frame && cursor < part.startFrame + part.frames && !cancel->load()) {
                const qint64 count = qMin(kIndexStride, qMin(frame - cursor, part.startFrame + part.frames - cursor));
                const QByteArray bytes = input.read(count * kFrameBytes);
                if (bytes.size() != count * kFrameBytes) {
                    error = QStringLiteral("回放分片被截断或读取失败: %1").arg(part.path);
                    break;
                }
                for (qint64 f = 0; f < count; ++f, ++cursor) {
                    if (cursor % kIndexStride == 0) added.insert(cursor, timeline);
                    advanceFileFrame(timeline, bytes.constData() + f * kFrameBytes,
                                     frameIsInvalid(invalidRanges, cursor));
                }
            }
            if (!error.isEmpty()) break;
        }
        if (cancel->load() || !self) return;
        QMetaObject::invokeMethod(self, [self, generation, frame, timeline, added, error]() {
            if (!self || self->m_seekGeneration != generation) return;
            if (!error.isEmpty()) { self->failPlayback(error); return; }
            for (auto it = added.cbegin(); it != added.cend(); ++it)
                self->m_checkpoints.insert(it.key(), it.value());
            self->applySeek(frame, timeline);
        }, Qt::QueuedConnection);
    });
}

void ReplayController::seekFraction(double frac) { seekFrame(static_cast<qint64>(qBound(0.0, frac, 1.0) * m_totalFrames)); }
void ReplayController::skipSeconds(double seconds) { seekFrame(m_curFrame + static_cast<qint64>(seconds * m_fs)); }
void ReplayController::jumpToStart() { seekFrame(0); }
void ReplayController::jumpToEnd() { seekFrame(m_totalFrames); }

bool ReplayController::loadParts(const QString &path, double fallbackRate) {
    SessionInput input;
    if (!SessionManifest::resolveInput(path, fallbackRate, &input, &m_error)) return false;
    SessionManifestData manifest;
    const bool manifestRead = input.hasManifest && SessionManifest::read(path, &manifest);
    m_sourceProvenance = QJsonObject{
        {"has_manifest", input.hasManifest}, {"frame_validity_known", input.frameValidityKnown},
        {"integrity_complete", input.integrityComplete}, {"integrity_unknown", input.integrityUnknown},
        {"original_complete", manifestRead && manifest.complete},
        {"ignored_tail_bytes", QString::number(input.ignoredTailBytes)},
        {"warning", input.warning}};
    m_parts = input.parts;
    m_metadata = input.metadata;
    m_frameValidityKnown = input.frameValidityKnown;
    m_integrityComplete = input.integrityComplete;
    m_integrityUnknown = input.integrityUnknown;
    m_invalidFrameRanges = input.invalidFrameRanges;
    m_totalFrames = input.totalFrames;
    m_fs = input.metadata.sampleRate;
    m_tdmKnown = input.hasManifest && input.metadata.tdmKnown;
    m_tdmEnabled = input.metadata.tdmEnabled;
    m_tdmEvenFirst = input.metadata.tdmEvenFirst;
    m_warning = input.warning;
    m_readerTimeline.reset(input.metadata.frameOrigin);
    m_checkpoints.insert(0, m_readerTimeline);
    return true;
}

bool ReplayController::openPart(int index) {
    if (index < 0 || index >= m_parts.size()) return false;
    m_file.close();
    m_file.setFileName(m_parts[index].path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("无法打开回放分片 %1: %2").arg(m_file.fileName(), m_file.errorString());
        return false;
    }
    m_partIndex = index;
    return true;
}

QByteArray ReplayController::readFrames(qint64 count) {
    QByteArray out;
    while (count > 0 && m_partIndex >= 0 && m_partIndex < m_parts.size()) {
        const qint64 remaining = m_parts[m_partIndex].frames - m_file.pos() / m_frameBytes;
        if (remaining <= 0) {
            if (!openPart(m_partIndex + 1)) break;
            continue;
        }
        const qint64 take = qMin(count, remaining);
        const QByteArray chunk = m_file.read(take * m_frameBytes);
        if (chunk.size() != take * m_frameBytes) {
            m_error = QStringLiteral("回放分片被截断或读取失败: %1").arg(m_file.fileName());
            return {};
        }
        out.append(chunk);
        count -= take;
    }
    return out;
}

bool ReplayController::readNextBlock(qint64 maximumFrames, QByteArray *bytes, StreamBlockInfo *info) {
    if (!bytes || !info || !isOpen() || isPlaying() || m_seeking || !m_pendingChunk.isEmpty()) {
        m_error = QStringLiteral("离线读取需要独立、暂停且没有待发送块的输入");
        return false;
    }
    bytes->clear(); *info = {};
    if (!m_error.isEmpty()) return false;
    const qint64 count = qMin(qBound<qint64>(qint64(1), maximumFrames, qint64(4096)), m_totalFrames - m_curFrame);
    if (count <= 0) return true;
    *bytes = readFrames(count);
    if (!m_error.isEmpty() || bytes->size() != count * m_frameBytes) {
        if (m_error.isEmpty()) m_error = QStringLiteral("离线输入读取不完整");
        bytes->clear(); return false;
    }
    info->epoch = m_epoch;
    info->integrityUnknown = m_integrityUnknown;
    const bool hasMask = m_frameValidityKnown || !m_invalidFrameRanges.isEmpty();
    info->frameIndices.reserve(count);
    if (hasMask) info->frameValid.reserve(count);
    for (qint64 f = 0; f < count; ++f) {
        const bool invalid = frameIsInvalid(m_invalidFrameRanges, m_curFrame + f);
        if (hasMask) info->frameValid.push_back(!invalid);
        info->frameIndices.push_back(advanceFileFrame(m_readerTimeline,
            bytes->constData() + f * kFrameBytes, invalid));
    }
    m_curFrame += count;
    return true;
}

bool ReplayController::deliverPending() {
    if (m_pendingChunk.isEmpty()) return true;
    if (!m_queue->push(m_pendingChunk, false, nullptr, m_pendingInfo)) {
        if (!m_backpressured) { m_backpressured = true; emit backpressureChanged(true); }
        return false;
    }
    if (m_backpressured) { m_backpressured = false; emit backpressureChanged(false); }
    m_curFrame += m_pendingFrames;
    m_frameAccum = qMax(0.0, m_frameAccum - m_pendingFrames);
    m_pendingChunk.clear();
    m_pendingInfo = {};
    m_pendingFrames = 0;
    emit positionChanged(m_curFrame, m_totalFrames);
    return true;
}

void ReplayController::failPlayback(const QString &message) {
    m_error = message;
    m_seeking = false;
    pause();
    m_pendingChunk.clear();
    m_pendingInfo = {};
    m_pendingFrames = 0;
    emit errorOccurred(message);
}

void ReplayController::tick() {
    if (!m_file.isOpen() || m_seeking) return;
    if (!deliverPending()) { m_clock.restart(); return; }
    const qint64 elapsed = qMax<qint64>(1, m_clock.restart());
    m_frameAccum = qMin(m_frameAccum + m_fs * elapsed / 1000.0, qMax(4.0, m_fs * 0.25));
    while (m_curFrame < m_totalFrames) {
        const qint64 remaining = m_totalFrames - m_curFrame;
        // Bound individual queue items without capping the 1x sample rate.
        qint64 count = qMin<qint64>(4096, qMin<qint64>(static_cast<qint64>(m_frameAccum), remaining));
        if (count < remaining) count -= count % kTdmPhaseCount;
        if (count <= 0) break;
        m_pendingChunk = readFrames(count);
        if (!m_error.isEmpty() || m_pendingChunk.size() != count * m_frameBytes) {
            failPlayback(m_error.isEmpty() ? QStringLiteral("回放数据读取不完整") : m_error);
            return;
        }
        m_pendingFrames = count;
        m_pendingInfo.epoch = m_epoch;
        m_pendingInfo.integrityUnknown = m_integrityUnknown;
        const bool hasMask = m_frameValidityKnown || !m_invalidFrameRanges.isEmpty();
        if (hasMask) m_pendingInfo.frameValid.reserve(static_cast<qsizetype>(count));
        m_pendingInfo.frameIndices.reserve(static_cast<qsizetype>(count));
        for (qint64 f = 0; f < count; ++f) {
            if ((m_curFrame + f) % kIndexStride == 0) m_checkpoints.insert(m_curFrame + f, m_readerTimeline);
            const bool invalid = frameIsInvalid(m_invalidFrameRanges, m_curFrame + f);
            if (hasMask) m_pendingInfo.frameValid.push_back(!invalid);
            m_pendingInfo.frameIndices.push_back(advanceFileFrame(
                m_readerTimeline, m_pendingChunk.constData() + f * kFrameBytes, invalid));
        }
        if (!deliverPending()) break;
    }
    if (m_curFrame >= m_totalFrames && m_pendingChunk.isEmpty()) {
        pause();
        emit finished();
    }
}

}  // namespace ccv2
