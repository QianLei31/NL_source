#pragma once

#include <QFile>
#include <QMutex>
#include <QString>
#include <QVector>

#include "io/session_manifest.h"

namespace ccv2 {

class SessionRecorder {
public:
    QString start(const QString &baseDir,
                  const QString &folderName,
                  qint64 maxBytesPerFile,
                  const SessionMetadata &metadata);
    bool write(const QByteArray &rawBytes);
    bool setFrameOrigin(qint64 origin);
    bool stop(bool complete = true,
              const QString &reason = QStringLiteral("user_stop"),
              qint64 ingressDroppedFrames = 0,
              qint64 recordingDroppedFrames = 0,
              const SessionIntegrity &integrity = {});

    static qint64 freeSpaceBytes(const QString &dir);
    QString folderPath() const { return m_folderPath; }
    qint64 totalBytes() const { return m_totalBytes; }
    const QVector<SessionPartInfo> &parts() const { return m_parts; }

private:
    bool openPart();
    bool writeManifest(bool active,
                       bool complete,
                       const QString &reason,
                       qint64 ingressDroppedFrames,
                       qint64 recordingDroppedFrames,
                       const SessionIntegrity &integrity = {});
    static QString uniqueFolderPath(const QString &baseDir, const QString &folderName);

    QFile m_file;
    QMutex m_mutex;
    QString m_folderPath;
    qint64 m_maxBytesPerFile = 0;
    qint64 m_currentBytes = 0;
    qint64 m_totalBytes = 0;
    int m_partIndex = 0;
    SessionMetadata m_metadata;
    QVector<SessionPartInfo> m_parts;
};

}  // namespace ccv2
