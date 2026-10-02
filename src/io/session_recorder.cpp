#include "io/session_recorder.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTextStream>

#include "core/constants.h"

namespace ccv2 {

QString SessionRecorder::uniqueFolderPath(const QString &baseDir, const QString &folderName)
{
    QDir base(baseDir);
    const QString requested = folderName.trimmed().isEmpty()
                                  ? QStringLiteral("v6_%1").arg(
                                        QDateTime::currentDateTime().toString(
                                            QStringLiteral("yyyyMMdd_HHmmss")))
                                  : folderName.trimmed();
    QString candidate = base.filePath(requested);
    int suffix = 1;
    while (QFileInfo::exists(candidate)) {
        candidate = base.filePath(
            QStringLiteral("%1_%2").arg(requested).arg(suffix++, 3, 10, QLatin1Char('0')));
    }
    return candidate;
}

QString SessionRecorder::start(const QString &baseDir,
                               const QString &folderName,
                               qint64 maxBytesPerFile,
                               const SessionMetadata &metadata) {
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_file.close();
    }

    QDir base(baseDir);
    if (!base.exists()) {
        if (!base.mkpath(QStringLiteral("."))) {
            return QString();
        }
    }

    m_folderPath = uniqueFolderPath(base.absolutePath(), folderName);
    if (!QDir().mkpath(m_folderPath)) {
        return QString();
    }
    m_maxBytesPerFile =
        maxBytesPerFile > 0
            ? qMax<qint64>(kFrameBytes,
                           (maxBytesPerFile / kFrameBytes) * kFrameBytes)
            : 0;
    m_partIndex = 0;
    m_currentBytes = 0;
    m_totalBytes = 0;
    m_parts.clear();
    m_invalidFrameRanges.clear();
    m_frameValidityKnown = true;
    m_integrityUnknown = false;
    m_metadata = metadata;
    if (m_metadata.createdAt.isEmpty()) {
        m_metadata.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    }

    if (!openPart()) {
        return QString();
    }

    QFile meta(QDir(m_folderPath).filePath(QStringLiteral("metadata.txt")));
    if (meta.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&meta);
        ts << "created_at: " << m_metadata.createdAt << "\n";
        ts << "format: int32 little-endian interleaved 256ch\n";
        ts << "frame_bytes: " << kFrameBytes << "\n";
        ts << "sample_rate_hz: " << m_metadata.sampleRate << "\n";
        ts << "adc_bits: " << kAdcBits << "\n";
        ts << "vref_volts: " << kVref << "\n";
        ts << "timestamp: bits[31:12]\n";
        ts << "split_bytes: " << m_maxBytesPerFile << "\n";
    }

    if (!writeManifest(true, false, QStringLiteral("recording"), 0, 0)) {
        m_file.close();
        return QString();
    }
    return m_folderPath;
}

bool SessionRecorder::openPart() {
    if (m_file.isOpen()) {
        // QFile buffers writes: on a full disk write() can succeed while
        // flush() is what actually fails. A failed rotation flush means the
        // previous part is truncated — treat it as a write failure.
        const bool flushOk = m_file.flush();
        m_file.close();
        if (!flushOk) {
            return false;
        }
    }
    const QString name = (m_maxBytesPerFile > 0)
        ? QStringLiteral("ADC_DATA_%1.bin").arg(m_partIndex, 4, 10, QLatin1Char('0'))
        : QStringLiteral("ADC_DATA.bin");
    m_file.setFileName(QDir(m_folderPath).filePath(name));
    m_currentBytes = 0;
    if (!m_file.open(QIODevice::WriteOnly)) {
        return false;
    }
    SessionPartInfo part;
    part.fileName = name;
    m_parts.push_back(part);
    return true;
}

bool SessionRecorder::write(const QByteArray &rawBytes, const StreamBlockInfo &info) {
    QMutexLocker locker(&m_mutex);
    if (!m_file.isOpen() || rawBytes.isEmpty() ||
        rawBytes.size() % kFrameBytes != 0 ||
        (!info.frameValid.isEmpty() && info.frameValid.size() != rawBytes.size() / kFrameBytes)) {
        return false;
    }

    // Keep metadata bounded. Refuse a block before writing it if loss toggles
    // would exceed the representable range budget; never silently drop a mask.
    qsizetype rangeCount = m_invalidFrameRanges.size();
    bool invalidBefore = !m_invalidFrameRanges.isEmpty() &&
        m_invalidFrameRanges.last().startFrame + m_invalidFrameRanges.last().frameCount ==
            m_totalBytes / kFrameBytes;
    for (const bool valid : info.frameValid) {
        if (!valid && !invalidBefore && ++rangeCount > SessionManifest::kMaxValidityRanges)
            return false;
        invalidBefore = !valid;
    }
    m_frameValidityKnown = m_frameValidityKnown && !info.frameValid.isEmpty() && !info.integrityUnknown;
    m_integrityUnknown = m_integrityUnknown || info.integrityUnknown;
    qint64 offset = 0;
    while (offset < rawBytes.size()) {
        if (m_maxBytesPerFile > 0 && m_currentBytes >= m_maxBytesPerFile) {
            ++m_partIndex;
            if (!openPart()) {
                return false;
            }
            if (!writeManifest(true,
                               false,
                               QStringLiteral("recording"),
                               0,
                               0)) {
                return false;
            }
        }

        qint64 bytesToWrite = rawBytes.size() - offset;
        if (m_maxBytesPerFile > 0) {
            bytesToWrite =
                qMin(bytesToWrite, m_maxBytesPerFile - m_currentBytes);
        }
        bytesToWrite = (bytesToWrite / kFrameBytes) * kFrameBytes;
        if (bytesToWrite <= 0) {
            return false;
        }

        const qint64 written = m_file.write(rawBytes.constData() + offset, bytesToWrite);
        if (written != bytesToWrite) {
            return false;
        }
        const qint64 firstRecordedFrame = m_totalBytes / kFrameBytes;
        const qint64 firstInputFrame = offset / kFrameBytes;
        if (!info.frameValid.isEmpty()) {
            for (qint64 f = 0; f < written / kFrameBytes; ++f) {
                if (info.frameValid.at(firstInputFrame + f)) continue;
                const qint64 frame = firstRecordedFrame + f;
                if (!m_invalidFrameRanges.isEmpty() &&
                    m_invalidFrameRanges.last().startFrame + m_invalidFrameRanges.last().frameCount == frame)
                    ++m_invalidFrameRanges.last().frameCount;
                else
                    m_invalidFrameRanges.push_back({frame, 1});
            }
        }
        offset += written;
        m_currentBytes += written;
        m_totalBytes += written;
        SessionPartInfo &part = m_parts.last();
        part.bytes += written;
        part.frames += written / kFrameBytes;
    }
    return true;
}

void SessionRecorder::setAnalysisMetadata(const QJsonObject &metadata) {
    QMutexLocker locker(&m_mutex);
    m_metadata.neuralAnalysis = metadata;
}

bool SessionRecorder::setFrameOrigin(qint64 origin) {
    QMutexLocker locker(&m_mutex);
    m_metadata.frameOrigin = qMax<qint64>(0, origin);
    return writeManifest(true, false, QStringLiteral("recording"), 0, 0);
}

bool SessionRecorder::writeManifest(bool active,
                                    bool complete,
                                    const QString &reason,
                                    qint64 ingressDroppedFrames,
                                    qint64 recordingDroppedFrames,
                                    const SessionIntegrity &integrity)
{
    SessionManifestData manifest;
    manifest.metadata = m_metadata;
    manifest.parts = m_parts;
    manifest.totalBytes = m_totalBytes;
    manifest.totalFrames = m_totalBytes / kFrameBytes;
    manifest.ingressDroppedFrames = ingressDroppedFrames;
    manifest.recordingDroppedFrames = recordingDroppedFrames;
    manifest.integrity = integrity;
    manifest.frameValidityKnown = m_frameValidityKnown;
    manifest.integrityUnknown = m_integrityUnknown;
    manifest.invalidFrameRanges = m_invalidFrameRanges;
    manifest.active = active;
    manifest.complete = complete;
    manifest.stopReason = reason;
    if (!active) {
        manifest.endedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    }
    return SessionManifest::write(m_folderPath, manifest);
}

bool SessionRecorder::stop(bool complete,
                           const QString &reason,
                           qint64 ingressDroppedFrames,
                           qint64 recordingDroppedFrames,
                           const SessionIntegrity &integrity) {
    QMutexLocker locker(&m_mutex);
    bool flushOk = true;
    if (m_file.isOpen()) {
        // A failed final flush means tail data never reached the disk; the
        // manifest must not claim the session is complete with inflated byte
        // counts (replay/export would silently read truncated data).
        flushOk = m_file.flush();
        m_file.close();
    }
    if (!m_folderPath.isEmpty()) {
        const bool manifestOk =
            writeManifest(false,
                          complete && flushOk && integrity.clean(),
                          flushOk ? reason : QStringLiteral("final_flush_failed"),
                          ingressDroppedFrames,
                          recordingDroppedFrames, integrity);
        return manifestOk && flushOk;
    }
    return flushOk;
}

qint64 SessionRecorder::freeSpaceBytes(const QString &dir) {
    QFileInfo probe(QDir(dir).absolutePath());
    while (!probe.exists() && !probe.absolutePath().isEmpty() &&
           probe.absoluteFilePath() != probe.absolutePath()) {
        probe.setFile(probe.absolutePath());
    }
    QStorageInfo info(probe.absoluteFilePath());
    return info.isValid() ? info.bytesAvailable() : -1;
}

}  // namespace ccv2
