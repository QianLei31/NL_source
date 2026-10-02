#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ccv2 {

struct SessionPartInfo {
    QString fileName;
    qint64 bytes{0};
    qint64 frames{0};
};

// Half-open interval in concatenated recorded-file frames, never hardware or
// source indices. A split or seek therefore cannot move a validity boundary.
struct SessionFrameRange {
    qint64 startFrame{0};
    qint64 frameCount{0};
};

struct SessionMetadata {
    QString source{QStringLiteral("live")};
    QString host;
    int controlPort{0};
    int dataPort{0};
    QString command{QStringLiteral("ctre")};
    double sampleRate{20000.0};
    QString createdAt;
    qint64 frameOrigin{0};
    // Hardware TDM state at recording time. tdmKnown stays false for
    // manifests written before these fields existed. tdmEvenFirst is a legacy
    // field name whose V6 meaning is now true=display pair 0/2, false=pair 1/3.
    bool tdmKnown{false};
    bool tdmEnabled{false};
    bool tdmEvenFirst{true};
    QJsonObject neuralAnalysis;
};

struct SessionIntegrity {
    qint64 upstreamMissingFrames{0};
    qint64 repeatedFrames{0};
    qint64 irregularJumps{0};
    qint64 intraFrameMismatchFrames{0};
    bool clean() const {
        return upstreamMissingFrames == 0 && repeatedFrames == 0 &&
               irregularJumps == 0 && intraFrameMismatchFrames == 0;
    }
};

struct SessionManifestData {
    SessionMetadata metadata;
    QVector<SessionPartInfo> parts;
    qint64 totalBytes{0};
    qint64 totalFrames{0};
    qint64 ingressDroppedFrames{0};
    qint64 recordingDroppedFrames{0};
    SessionIntegrity integrity;
    bool frameValidityKnown{false};
    bool integrityUnknown{false};
    QVector<SessionFrameRange> invalidFrameRanges;
    bool active{false};
    bool complete{false};
    QString stopReason;
    QString endedAt;
};

struct SessionInputPart {
    QString path;
    qint64 frames{0};
    qint64 startFrame{0};
};

struct SessionInput {
    QVector<SessionInputPart> parts;
    SessionMetadata metadata;
    bool hasManifest{false};
    bool frameValidityKnown{false};
    bool integrityUnknown{false};
    bool integrityComplete{false};
    QVector<SessionFrameRange> invalidFrameRanges;
    qint64 totalFrames{0};
    qint64 ignoredTailBytes{0};
    QString warning;
};

class SessionManifest {
public:
    static constexpr qsizetype kMaxValidityRanges = 65536;
    static bool resolveInput(const QString &path, double fallbackSampleRate,
                             SessionInput *input, QString *error = nullptr);
    static bool write(const QString &folderPath,
                      const SessionManifestData &data,
                      QString *error = nullptr);
    static bool read(const QString &folderOrManifest,
                     SessionManifestData *data,
                     QString *error = nullptr);
};

}  // namespace ccv2
