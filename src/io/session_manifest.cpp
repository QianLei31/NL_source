#include "io/session_manifest.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>

#include "core/constants.h"

namespace ccv2 {

namespace {

QString manifestPath(const QString &folderOrManifest)
{
    const QFileInfo info(folderOrManifest);
    if (info.isDir()) {
        return QDir(info.absoluteFilePath()).filePath(QStringLiteral("session.json"));
    }
    if (info.fileName().compare(QStringLiteral("session.json"), Qt::CaseInsensitive) == 0) {
        return info.absoluteFilePath();
    }
    return QDir(info.absolutePath()).filePath(QStringLiteral("session.json"));
}

qint64 jsonInt64(const QJsonObject &object, const QString &key, qint64 fallback = 0)
{
    const QJsonValue value = object.value(key);
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : fallback;
    }
    return value.isDouble() ? value.toInteger(fallback) : fallback;
}

// JSON doubles above 2^53 cannot encode every integer; writers use decimal
// strings, and readers reject rounded, fractional, or overflowing coordinates.
bool exactNonnegativeInt(const QJsonValue &value, qint64 *result)
{
    if (value.isString()) {
        const QString text = value.toString();
        if (text.isEmpty()) return false;
        for (const QChar c : text) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
        bool ok = false;
        *result = text.toLongLong(&ok);
        return ok && *result >= 0;
    }
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0 ||
        number != std::floor(number)) return false;
    *result = static_cast<qint64>(number);
    return true;
}

bool validRanges(const QVector<SessionFrameRange> &ranges, qint64 totalFrames)
{
    if (totalFrames < 0 || ranges.size() > SessionManifest::kMaxValidityRanges) return false;
    qint64 previousEnd = 0;
    for (const auto &range : ranges) {
        if (range.startFrame < previousEnd || range.frameCount <= 0 ||
            range.startFrame > totalFrames || range.frameCount > totalFrames - range.startFrame)
            return false;
        previousEnd = range.startFrame + range.frameCount;
    }
    return true;
}

}  // namespace

bool SessionManifest::write(const QString &folderPath,
                            const SessionManifestData &data,
                            QString *error)
{
    if (!validRanges(data.invalidFrameRanges, data.totalFrames)) {
        if (error) *error = QStringLiteral("session.json invalid frame validity ranges");
        return false;
    }
    QJsonObject format;
    format.insert(QStringLiteral("channels"), kChannelsTotal);
    format.insert(QStringLiteral("bytes_per_point"), kBytesPerPoint);
    format.insert(QStringLiteral("frame_bytes"), kFrameBytes);
    format.insert(QStringLiteral("adc_bits"), kAdcBits);
    format.insert(QStringLiteral("vref_volts"), kVref);
    format.insert(QStringLiteral("timestamp_shift"), kTimestampShift);
    format.insert(QStringLiteral("timestamp_bits"), 20);
    format.insert(QStringLiteral("endianness"), QStringLiteral("little"));
    format.insert(QStringLiteral("layout"), QStringLiteral("timestamp[31:12],adc[11:0]"));

    QJsonObject source;
    source.insert(QStringLiteral("type"), data.metadata.source);
    source.insert(QStringLiteral("host"), data.metadata.host);
    source.insert(QStringLiteral("control_port"), data.metadata.controlPort);
    source.insert(QStringLiteral("data_port"), data.metadata.dataPort);
    source.insert(QStringLiteral("command"), data.metadata.command);
    source.insert(QStringLiteral("sample_rate_hz"), data.metadata.sampleRate);
    source.insert(QStringLiteral("frame_origin"), QString::number(data.metadata.frameOrigin));
    source.insert(QStringLiteral("tdm_enabled"), data.metadata.tdmEnabled);
    source.insert(QStringLiteral("tdm_pair_02"), data.metadata.tdmEvenFirst);
    // Legacy key retained so older V6 builds can still open new recordings.
    source.insert(QStringLiteral("tdm_even_first"), data.metadata.tdmEvenFirst);

    QJsonArray parts;
    for (const SessionPartInfo &part : data.parts) {
        QJsonObject item;
        item.insert(QStringLiteral("file"), part.fileName);
        item.insert(QStringLiteral("bytes"), QString::number(part.bytes));
        item.insert(QStringLiteral("frames"), QString::number(part.frames));
        parts.append(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    if (!data.metadata.neuralAnalysis.isEmpty())
        root.insert(QStringLiteral("neural_analysis"), data.metadata.neuralAnalysis);
    if (data.frameValidityKnown || data.integrityUnknown || !data.invalidFrameRanges.isEmpty()) {
        QJsonObject validity;
        validity.insert(QStringLiteral("version"), 1);
        validity.insert(QStringLiteral("coordinate_space"), QStringLiteral("recorded_frames"));
        validity.insert(QStringLiteral("known"), data.frameValidityKnown);
        validity.insert(QStringLiteral("integrity_unknown"), data.integrityUnknown);
        QJsonArray ranges;
        for (const auto &range : data.invalidFrameRanges) {
            QJsonObject item;
            item.insert(QStringLiteral("start_frame"), QString::number(range.startFrame));
            item.insert(QStringLiteral("frame_count"), QString::number(range.frameCount));
            ranges.append(item);
        }
        validity.insert(QStringLiteral("invalid_ranges"), ranges);
        root.insert(QStringLiteral("frame_validity"), validity);
    }
    root.insert(QStringLiteral("created_at"), data.metadata.createdAt);
    root.insert(QStringLiteral("ended_at"), data.endedAt);
    root.insert(QStringLiteral("active"), data.active);
    root.insert(QStringLiteral("complete"), data.complete);
    root.insert(QStringLiteral("stop_reason"), data.stopReason);
    root.insert(QStringLiteral("format"), format);
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("parts"), parts);
    root.insert(QStringLiteral("total_bytes"), QString::number(data.totalBytes));
    root.insert(QStringLiteral("total_frames"), QString::number(data.totalFrames));
    root.insert(QStringLiteral("ingress_dropped_frames"),
                QString::number(data.ingressDroppedFrames));
    root.insert(QStringLiteral("recording_dropped_frames"),
                QString::number(data.recordingDroppedFrames));
    root.insert(QStringLiteral("upstream_missing_frames"), QString::number(data.integrity.upstreamMissingFrames));
    root.insert(QStringLiteral("timestamp_repeated_frames"), QString::number(data.integrity.repeatedFrames));
    root.insert(QStringLiteral("timestamp_irregular_jumps"), QString::number(data.integrity.irregularJumps));
    root.insert(QStringLiteral("intra_frame_mismatch_frames"), QString::number(data.integrity.intraFrameMismatchFrames));

    QDir().mkpath(folderPath);
    QSaveFile file(QDir(folderPath).filePath(QStringLiteral("session.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
        if (error) *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool SessionManifest::read(const QString &folderOrManifest,
                           SessionManifestData *data,
                           QString *error)
{
    if (!data) {
        if (error) *error = QStringLiteral("null manifest output");
        return false;
    }

    *data = SessionManifestData{};
    QFile file(manifestPath(folderOrManifest));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = parseError.errorString();
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt(-1) != 1) {
        if (error) *error = QStringLiteral("不支持的 session.json schema_version");
        return false;
    }
    const QJsonValue formatValue = root.value(QStringLiteral("format"));
    if (!formatValue.isObject()) {
        if (error) *error = QStringLiteral("session.json 缺少 format");
        return false;
    }
    const QJsonObject format = formatValue.toObject();
    if (format.value(QStringLiteral("channels")).toInt(-1) != kChannelsTotal ||
        format.value(QStringLiteral("bytes_per_point")).toInt(-1) != kBytesPerPoint ||
        format.value(QStringLiteral("frame_bytes")).toInt(-1) != kFrameBytes ||
        format.value(QStringLiteral("endianness")).toString().compare(
            QStringLiteral("little"), Qt::CaseInsensitive) != 0) {
        if (error) {
            *error = QStringLiteral("session.json 数据格式与当前 256CH/32-bit little-endian 帧格式不兼容");
        }
        return false;
    }

    const QJsonObject source = root.value(QStringLiteral("source")).toObject();
    data->metadata.source = source.value(QStringLiteral("type")).toString(QStringLiteral("live"));
    data->metadata.host = source.value(QStringLiteral("host")).toString();
    data->metadata.controlPort = source.value(QStringLiteral("control_port")).toInt();
    data->metadata.dataPort = source.value(QStringLiteral("data_port")).toInt();
    data->metadata.command = source.value(QStringLiteral("command")).toString(QStringLiteral("ctre"));
    data->metadata.sampleRate = source.value(QStringLiteral("sample_rate_hz")).toDouble(20000.0);
    data->metadata.frameOrigin = qMax<qint64>(0, jsonInt64(source, QStringLiteral("frame_origin")));
    if (data->metadata.sampleRate <= 0.0) {
        if (error) *error = QStringLiteral("session.json 的 sample_rate_hz 无效");
        return false;
    }
    data->metadata.tdmKnown = source.contains(QStringLiteral("tdm_enabled"));
    data->metadata.tdmEnabled =
        source.value(QStringLiteral("tdm_enabled")).toBool(false);
    data->metadata.tdmEvenFirst = source.contains(QStringLiteral("tdm_pair_02"))
                                      ? source.value(QStringLiteral("tdm_pair_02")).toBool(true)
                                      : source.value(QStringLiteral("tdm_even_first")).toBool(true);
    const auto analysis = root.value(QStringLiteral("neural_analysis"));
    if (!analysis.isUndefined() && !analysis.isObject()) {
        if (error) *error = QStringLiteral("session.json neural_analysis must be an object");
        return false;
    }
    data->metadata.neuralAnalysis = analysis.toObject();
    data->metadata.createdAt = root.value(QStringLiteral("created_at")).toString();
    data->endedAt = root.value(QStringLiteral("ended_at")).toString();
    data->active = root.value(QStringLiteral("active")).toBool(false);
    data->complete = root.value(QStringLiteral("complete")).toBool(false);
    data->stopReason = root.value(QStringLiteral("stop_reason")).toString();
    data->totalBytes = jsonInt64(root, QStringLiteral("total_bytes"));
    data->totalFrames = jsonInt64(root, QStringLiteral("total_frames"));
    data->ingressDroppedFrames =
        jsonInt64(root, QStringLiteral("ingress_dropped_frames"));
    data->recordingDroppedFrames =
        jsonInt64(root, QStringLiteral("recording_dropped_frames"));
    data->integrity.upstreamMissingFrames = jsonInt64(root, QStringLiteral("upstream_missing_frames"));
    data->integrity.repeatedFrames = jsonInt64(root, QStringLiteral("timestamp_repeated_frames"));
    data->integrity.irregularJumps = jsonInt64(root, QStringLiteral("timestamp_irregular_jumps"));
    data->integrity.intraFrameMismatchFrames = jsonInt64(root, QStringLiteral("intra_frame_mismatch_frames"));

    const auto validityValue = root.value(QStringLiteral("frame_validity"));
    if (!validityValue.isUndefined()) {
        const auto validity = validityValue.toObject();
        const auto rangesValue = validity.value(QStringLiteral("invalid_ranges"));
        if (!validityValue.isObject() || validity.value(QStringLiteral("version")) != QJsonValue(1) ||
            validity.value(QStringLiteral("coordinate_space")).toString() != QStringLiteral("recorded_frames") ||
            !validity.value(QStringLiteral("known")).isBool() || !rangesValue.isArray() ||
            rangesValue.toArray().size() > kMaxValidityRanges ||
            (validity.contains(QStringLiteral("integrity_unknown")) &&
             !validity.value(QStringLiteral("integrity_unknown")).isBool())) {
            if (error) *error = QStringLiteral("session.json malformed or unsupported frame_validity");
            return false;
        }
        qint64 exactTotalFrames = 0;
        if (!exactNonnegativeInt(root.value(QStringLiteral("total_frames")), &exactTotalFrames)) {
            if (error) *error = QStringLiteral("session.json invalid total_frames for frame validity coordinates");
            return false;
        }
        data->totalFrames = exactTotalFrames;
        data->frameValidityKnown = validity.value(QStringLiteral("known")).toBool();
        data->integrityUnknown = validity.value(QStringLiteral("integrity_unknown")).toBool();
        for (const auto &value : rangesValue.toArray()) {
            const auto object = value.toObject();
            SessionFrameRange range;
            if (!value.isObject() ||
                !exactNonnegativeInt(object.value(QStringLiteral("start_frame")), &range.startFrame) ||
                !exactNonnegativeInt(object.value(QStringLiteral("frame_count")), &range.frameCount)) {
                if (error) *error = QStringLiteral("session.json malformed frame validity interval");
                return false;
            }
            data->invalidFrameRanges.push_back(range);
        }
        if (!validRanges(data->invalidFrameRanges, data->totalFrames)) {
            if (error) *error = QStringLiteral("session.json invalid, overlapping or out-of-bounds frame validity intervals");
            return false;
        }
    }

    // A partial mask never proves the remaining frames valid. In particular,
    // old complete=true manifests can still contain unlocated drop padding.
    data->integrityUnknown = data->integrityUnknown || (!data->frameValidityKnown &&
        (data->ingressDroppedFrames != 0 || data->recordingDroppedFrames != 0 ||
         !data->integrity.clean() || !data->complete || !data->invalidFrameRanges.isEmpty()));

    data->parts.clear();
    const QJsonValue partsValue = root.value(QStringLiteral("parts"));
    if (!partsValue.isArray()) {
        if (error) *error = QStringLiteral("session.json 缺少 parts 数组");
        return false;
    }
    const QJsonArray parts = partsValue.toArray();
    for (const QJsonValue &value : parts) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("session.json 的 parts 项不是对象");
            return false;
        }
        const QJsonObject item = value.toObject();
        SessionPartInfo part;
        part.fileName = item.value(QStringLiteral("file")).toString();
        part.bytes = jsonInt64(item, QStringLiteral("bytes"));
        part.frames = jsonInt64(item, QStringLiteral("frames"));
        const QFileInfo partNameInfo(part.fileName);
        if (part.fileName.isEmpty() || partNameInfo.isAbsolute() ||
            part.fileName.contains(QStringLiteral(".."))) {
            if (error) *error = QStringLiteral("session.json 包含无效分片路径");
            return false;
        }
        data->parts.push_back(part);
    }
    if (data->parts.isEmpty()) {
        if (error) *error = QStringLiteral("session.json 没有可用分片");
        return false;
    }
    return true;
}

bool SessionManifest::resolveInput(const QString &path, double fallbackSampleRate,
                                   SessionInput *input, QString *error)
{
    if (!input) return false;
    *input = {};
    input->metadata.sampleRate = qMax(1.0, fallbackSampleRate);
    const QFileInfo selected(path);
    const bool explicitSession = selected.isDir() ||
        selected.fileName().compare(QStringLiteral("session.json"), Qt::CaseInsensitive) == 0;
    const QString candidate = manifestPath(path);
    SessionManifestData manifest;
    if (QFileInfo::exists(candidate)) {
        if (!read(candidate, &manifest, error)) return false;
        const QDir folder(QFileInfo(candidate).absolutePath());
        bool member = explicitSession;
        for (const auto &part : manifest.parts) {
            if (QFileInfo(folder.filePath(part.fileName)).canonicalFilePath() ==
                    selected.canonicalFilePath() && !selected.canonicalFilePath().isEmpty()) {
                member = true;
            }
        }
        if (member) {
            input->hasManifest = true;
            input->metadata = manifest.metadata;
            input->frameValidityKnown = manifest.frameValidityKnown;
            input->invalidFrameRanges = manifest.invalidFrameRanges;
            const bool hasLoss = manifest.ingressDroppedFrames != 0 ||
                manifest.recordingDroppedFrames != 0 || !manifest.integrity.clean();
            input->integrityUnknown = manifest.integrityUnknown ||
                (!manifest.frameValidityKnown && (hasLoss || !manifest.complete));
            input->integrityComplete = manifest.complete && manifest.frameValidityKnown &&
                !input->integrityUnknown && !hasLoss && manifest.invalidFrameRanges.isEmpty();
            bool fileCoordinatesMatch = true;
            if (!manifest.complete) input->warning = QStringLiteral("该 Session 数据可能不完整");
            for (const auto &part : manifest.parts) {
                const QFileInfo info(folder.filePath(part.fileName));
                if (!info.isFile()) {
                    if (error) *error = QStringLiteral("Session 分片不存在: %1").arg(info.absoluteFilePath());
                    return false;
                }
                if (manifest.complete &&
                    (info.size() != part.bytes || info.size() / kFrameBytes != part.frames)) {
                    if (error) *error = QStringLiteral("Session 分片大小与元数据不一致: %1").arg(info.fileName());
                    return false;
                }
                const qint64 frames = info.size() / kFrameBytes;
                fileCoordinatesMatch = fileCoordinatesMatch && frames == part.frames;
                input->parts.push_back({info.absoluteFilePath(), frames, input->totalFrames});
                input->totalFrames += frames;
                input->ignoredTailBytes += info.size() % kFrameBytes;
            }
            if (!fileCoordinatesMatch || input->totalFrames != manifest.totalFrames) {
                // An interrupted/truncated part shifts subsequent global offsets.
                // Never apply a stale mask to different samples.
                input->frameValidityKnown = false;
                input->integrityUnknown = true;
                input->integrityComplete = false;
                input->invalidFrameRanges.clear();
            }
            if (!input->frameValidityKnown || input->integrityUnknown)
                input->warning += QStringLiteral(" 帧有效性未知；缺少可靠的逐帧完整性信息");
            if (input->integrityUnknown)
                input->warning += QStringLiteral(" 检测已禁用，避免将未知丢帧占位数据当作神经信号");
            else if (!input->invalidFrameRanges.isEmpty())
                input->warning += QStringLiteral(" 已标记无效帧，神经检测将跳过这些帧");
        }
    } else if (explicitSession) {
        if (error) *error = QStringLiteral("Session 缺少 session.json");
        return false;
    }
    if (!input->hasManifest) {
        if (!selected.isFile()) {
            if (error) *error = QStringLiteral("BIN 文件不存在: %1").arg(path);
            return false;
        }
        input->totalFrames = selected.size() / kFrameBytes;
        input->ignoredTailBytes = selected.size() % kFrameBytes;
        input->parts.push_back({selected.absoluteFilePath(), input->totalFrames, 0});
        input->warning = QStringLiteral("原始 BIN 无帧有效性元数据，完整性未知");
    }
    if (input->ignoredTailBytes > 0) {
        input->integrityComplete = false;
        input->warning += QStringLiteral(" 分片残尾已分别忽略 (%1 B)").arg(input->ignoredTailBytes);
    }
    if (input->totalFrames == 0) {
        if (error) *error = QStringLiteral("没有完整数据帧");
        return false;
    }
    return true;
}

}  // namespace ccv2
