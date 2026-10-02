#include "io/matlab_channel_exporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QtEndian>

#include <memory>
#include <limits>
#include <vector>
#include <array>

#include "core/constants.h"
#include "core/channel_routing.h"
#include "core/frame_timestamp_reconciler.h"
#include "io/session_manifest.h"

namespace ccv2 {

namespace {

constexpr int kFramesPerChunk = 8192;
constexpr int kElectrodeTotal = 1024;

QString channelFileName(int ch, bool rawWords)
{
    return rawWords
               ? QStringLiteral("ch%1_raw32le.bin").arg(ch, 3, 10, QChar('0'))
               : QStringLiteral("ch%1_i32le.bin").arg(ch, 3, 10, QChar('0'));
}

QString electrodeFileName(int ele)
{
    return QStringLiteral("ele%1_i32le.bin").arg(ele, 4, 10, QChar('0'));
}

MatlabChannelExportResult makeError(const QString &message, const QString &outputDir = QString())
{
    MatlabChannelExportResult result;
    result.error = message;
    result.outputDir = outputDir;
    return result;
}

class MultiPartInput {
public:
    bool open(const QVector<SessionInputPart> &paths, QString *error)
    {
        close();
        m_paths = paths;
        return openNext(error);
    }

    QByteArray read(qint64 maxBytes)
    {
        QByteArray result;
        if (!m_error.isEmpty()) return result;
        result.reserve(static_cast<int>(qMin<qint64>(maxBytes, std::numeric_limits<int>::max())));
        while (result.size() < maxBytes) {
            if (!m_file.isOpen()) {
                if (!openNext(&m_error)) break;
            }
            const qint64 wanted = qMin(maxBytes - result.size(), m_partBytes - m_file.pos());
            if (wanted == 0) { m_file.close(); continue; }
            const QByteArray part = m_file.read(wanted);
            if (part.size() != wanted) {
                m_error = QStringLiteral("分片被截断或读取失败: %1").arg(m_file.fileName());
                break;
            }
            if (!part.isEmpty()) {
                result.append(part);
            }
            if (m_file.pos() >= m_partBytes) {
                m_file.close();
            } else if (part.isEmpty()) {
                m_error = m_file.errorString();
                break;
            }
        }
        return result;
    }

    bool atEnd() const { return !m_file.isOpen() && m_nextIndex >= m_paths.size(); }
    QString errorString() const { return m_error; }
    void close()
    {
        if (m_file.isOpen()) m_file.close();
        m_paths.clear();
        m_nextIndex = 0;
        m_error.clear();
    }

private:
    bool openNext(QString *error)
    {
        if (m_nextIndex >= m_paths.size()) return false;
        const SessionInputPart part = m_paths[m_nextIndex++];
        m_file.setFileName(part.path);
        m_partBytes = part.frames * kFrameBytes;
        if (!m_file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法打开分片 %1: %2")
                                    .arg(m_file.fileName(), m_file.errorString());
            return false;
        }
        return true;
    }

    QVector<SessionInputPart> m_paths;
    qint64 m_partBytes{0};
    int m_nextIndex{0};
    QFile m_file;
    QString m_error;
};

bool writeTextFile(const QString &path, const QString &content, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QTextStream ts(&file);
    ts << content;
    ts.flush();
    if (ts.status() != QTextStream::Ok || !file.flush()) {
        if (error) *error = file.errorString();
        return false;
    }
    file.close();
    return true;
}

QString matlabLoaderScript(qint64 frameCount, double samplingRateHz)
{
    QString script = QString::fromLatin1(
R"(function data = load_adc_channels(folder)
%LOAD_ADC_CHANNELS Load all exported per-channel int32 binary files.
%   data = LOAD_ADC_CHANNELS(folder) returns a struct with ch000...ch255.
if nargin < 1 || isempty(folder)
    folder = fileparts(mfilename('fullpath'));
end

frame_count = __FRAME_COUNT__;
channel_count = __CHANNEL_COUNT__;
data = struct();
data.fs_hz = __FS_HZ__;
data.frame_count = frame_count;
data.channel_count = channel_count;

for ch = 0:(channel_count - 1)
    data.(sprintf('ch%03d', ch)) = read_adc_channel(folder, ch);
end
end
)");
    script.replace(QStringLiteral("__FRAME_COUNT__"), QString::number(frameCount));
    script.replace(QStringLiteral("__CHANNEL_COUNT__"), QString::number(kChannelsTotal));
    script.replace(QStringLiteral("__FS_HZ__"), QString::number(samplingRateHz, 'g', 17));
    return script;
}

QString matlabSingleChannelScript(qint64 frameCount,
                                  double samplingRateHz,
                                  bool rawWords)
{
    QString script = QString::fromLatin1(
R"(function samples = read_adc_channel(folder, ch)
%READ_ADC_CHANNEL Read one exported channel as an int32 column vector.
%   samples = READ_ADC_CHANNEL(folder, 239)
if nargin < 2
    error('Usage: samples = read_adc_channel(folder, ch)');
end
if isempty(folder)
    folder = fileparts(mfilename('fullpath'));
end
if ch < 0 || ch >= __CHANNEL_COUNT__ || fix(ch) ~= ch
    error('Channel must be an integer in [0, __CHANNEL_LAST__]');
end

frame_count = __FRAME_COUNT__;
fs_hz = __FS_HZ__; %#ok<NASGU>
file_name = fullfile(folder, sprintf('__FILE_PATTERN__', ch));
fid = fopen(file_name, 'rb', 'ieee-le');
if fid < 0
    error('Cannot open %s', file_name);
end
cleaner = onCleanup(@() fclose(fid));
samples = fread(fid, [frame_count, 1], '__MATLAB_TYPE__');
if numel(samples) ~= frame_count
    warning('Expected %d samples, got %d from %s', frame_count, numel(samples), file_name);
end
end
)");
    script.replace(QStringLiteral("__CHANNEL_COUNT__"), QString::number(kChannelsTotal));
    script.replace(QStringLiteral("__CHANNEL_LAST__"), QString::number(kChannelsTotal - 1));
    script.replace(QStringLiteral("__FRAME_COUNT__"), QString::number(frameCount));
    script.replace(QStringLiteral("__FS_HZ__"), QString::number(samplingRateHz, 'g', 17));
    script.replace(QStringLiteral("__FILE_PATTERN__"),
                   rawWords ? QStringLiteral("ch%03d_raw32le.bin")
                            : QStringLiteral("ch%03d_i32le.bin"));
    script.replace(QStringLiteral("__MATLAB_TYPE__"),
                   rawWords ? QStringLiteral("uint32=>uint32")
                            : QStringLiteral("int32=>int32"));
    return script;
}

QString metadataText(const QString &adcFile,
                     qint64 frameCount,
                     qint64 leftoverBytes,
                     double samplingRateHz,
                     bool rawWords)
{
    return QStringLiteral(
        "source_adc=%1\n"
        "export_mode=%2\n"
        "format=%3\n"
        "channel_count=%4\n"
        "frame_bytes=%5\n"
        "bytes_per_sample=%6\n"
        "frame_count=%7\n"
        "leftover_bytes_ignored=%8\n"
        "sampling_rate_hz=%9\n"
        "file_pattern=%10\n"
        "matlab_load_all=data = load_adc_channels(folder)\n"
        "matlab_load_one=ch239 = read_adc_channel(folder, 239)\n")
        .arg(QDir::toNativeSeparators(adcFile))
        .arg(rawWords ? QStringLiteral("raw32_words") : QStringLiteral("adc12"))
        .arg(rawWords ? QStringLiteral("uint32 little-endian original words")
                      : QStringLiteral("int32 little-endian low-12-bit ADC values"))
        .arg(kChannelsTotal)
        .arg(kFrameBytes)
        .arg(kBytesPerPoint)
        .arg(frameCount)
        .arg(leftoverBytes)
        .arg(samplingRateHz, 0, 'g', 17)
        .arg(rawWords ? QStringLiteral("ch###_raw32le.bin")
                      : QStringLiteral("ch###_i32le.bin"));
}

struct TdmOutput {
    int sourceChannel{-1};
    int localEle{-1};
    int globalEle{-1};
    int phase{-1};
    QString fileName;
};

QString matlabTdmLoaderScript(double rawSamplingRateHz)
{
    QString script = QString::fromLatin1(
R"(function data = load_tdm_ele_channels(folder)
%LOAD_TDM_ELE_CHANNELS Load all exported TDM-demuxed electrode binary files.
%   data = LOAD_TDM_ELE_CHANNELS(folder) returns a struct with ele0000...
if nargin < 1 || isempty(folder)
    folder = fileparts(mfilename('fullpath'));
end

data = struct();
data.raw_fs_hz = __RAW_FS_HZ__;
data.fs_hz = __TDM_FS_HZ__;
data.nominal_fs_hz = data.fs_hz;
data.export_mode = 'tdm_demux';
data.present_ele = [];
data.frame_index = cell(1, 4);
data.time_s = cell(1, 4);
for phase = 0:3
    [data.time_s{phase+1}, data.frame_index{phase+1}] = read_tdm_timeline(folder, phase);
end
data.is_uniform_phase = cellfun(@(idx) all(diff(idx) == 4), data.frame_index);
% Match eleN to data.time_s{mod(N,4)+1}; missing samples are not time-compressed.

for ele = 0:__ELECTRODE_LAST__
    file_name = fullfile(folder, sprintf('ele%04d_i32le.bin', ele));
    if exist(file_name, 'file')
        data.present_ele(end + 1) = ele; %#ok<AGROW>
        data.(sprintf('ele%04d', ele)) = read_tdm_ele(folder, ele);
    end
end
end
)");
    script.replace(QStringLiteral("__RAW_FS_HZ__"), QString::number(rawSamplingRateHz, 'g', 17));
    script.replace(QStringLiteral("__TDM_FS_HZ__"), QString::number(rawSamplingRateHz / kTdmPhaseCount, 'g', 17));
    script.replace(QStringLiteral("__ELECTRODE_LAST__"), QString::number(kElectrodeTotal - 1));
    return script;
}

QString matlabTdmSingleEleScript(double rawSamplingRateHz)
{
    QString script = QString::fromLatin1(
R"(function [samples, time_s, frame_index] = read_tdm_ele(folder, ele)
%READ_TDM_ELE Read one TDM-demuxed electrode as an int32 column vector.
%   samples = READ_TDM_ELE(folder, 239)
if nargin < 2
    error('Usage: samples = read_tdm_ele(folder, ele)');
end
if isempty(folder)
    folder = fileparts(mfilename('fullpath'));
end
if ele < 0 || ele > __ELECTRODE_LAST__ || fix(ele) ~= ele
    error('Electrode must be an integer in [0, __ELECTRODE_LAST__]');
end

raw_fs_hz = __RAW_FS_HZ__; %#ok<NASGU>
fs_hz = __TDM_FS_HZ__; %#ok<NASGU>
file_name = fullfile(folder, sprintf('ele%04d_i32le.bin', ele));
fid = fopen(file_name, 'rb', 'ieee-le');
if fid < 0
    error('Cannot open %s', file_name);
end
cleaner = onCleanup(@() fclose(fid));
samples = fread(fid, Inf, 'int32=>int32');
if nargout > 1
    [time_s, frame_index] = read_tdm_timeline(folder, mod(ele, 4));
    if numel(frame_index) ~= numel(samples)
        error('Sample/timeline size mismatch for electrode %d', ele);
    end
end
end
)");
    script.replace(QStringLiteral("__RAW_FS_HZ__"), QString::number(rawSamplingRateHz, 'g', 17));
    script.replace(QStringLiteral("__TDM_FS_HZ__"), QString::number(rawSamplingRateHz / kTdmPhaseCount, 'g', 17));
    script.replace(QStringLiteral("__ELECTRODE_LAST__"), QString::number(kElectrodeTotal - 1));
    return script;
}

QString matlabTdmTimelineScript(double rawFs, qint64 frameOrigin)
{
    QString script = QString::fromLatin1(R"(function [time_s, frame_index] = read_tdm_timeline(folder, phase)
%READ_TDM_TIMELINE Shared sample positions for all electrodes of one TDM phase.
if phase < 0 || phase > 3 || fix(phase) ~= phase
    error('Phase must be 0, 1, 2, or 3');
end
name = fullfile(folder, sprintf('tdm_phase%d_frames_i64le.bin', phase));
fid = fopen(name, 'rb', 'ieee-le');
if fid < 0, error('Cannot open %s', name); end
cleanup = onCleanup(@() fclose(fid));
frame_index = fread(fid, Inf, 'int64=>int64');
time_s = double(frame_index - int64(__ORIGIN__)) / __RAW_FS__;
end
)");
    script.replace(QStringLiteral("__ORIGIN__"), QString::number(frameOrigin));
    script.replace(QStringLiteral("__RAW_FS__"), QString::number(rawFs, 'g', 17));
    return script;
}

QString tdmMapText(const std::vector<TdmOutput> &outputs)
{
    QString text = QStringLiteral("global_ele\tadc_channel\tlocal_ele\ttdm_phase\tfile_name\n");
    for (const TdmOutput &output : outputs) {
        text += QStringLiteral("%1\t%2\t%3\t%4\t%5\n")
                    .arg(output.globalEle)
                    .arg(output.sourceChannel)
                    .arg(output.localEle)
                    .arg(output.phase)
                    .arg(output.fileName);
    }
    return text;
}

QString metadataTdmText(const QString &adcFile,
                        qint64 frameCount,
                        qint64 leftoverBytes,
                        double samplingRateHz,
                        int outputCount)
{
    return QStringLiteral(
        "source_adc=%1\n"
        "export_mode=tdm_demux\n"
        "format=int32 little-endian per-electrode binary\n"
        "adc_channel_count=%2\n"
        "tdm_output_count=%3\n"
        "electrode_span=%4\n"
        "frame_bytes=%5\n"
        "bytes_per_sample=%6\n"
        "raw_frame_count=%7\n"
        "leftover_bytes_ignored=%8\n"
        "raw_sampling_rate_hz=%9\n"
        "tdm_sampling_rate_hz=%10\n"
        "tdm_phase_count=%11\n"
        "file_pattern=ele####_i32le.bin\n"
        "map_file=tdm_map.tsv\n"
        "matlab_load_all=data = load_tdm_ele_channels(folder)\n"
        "matlab_load_one=ele0000 = read_tdm_ele(folder, 0)\n")
        .arg(QDir::toNativeSeparators(adcFile))
        .arg(kChannelsTotal)
        .arg(outputCount)
        .arg(kElectrodeTotal)
        .arg(kFrameBytes)
        .arg(kBytesPerPoint)
        .arg(frameCount)
        .arg(leftoverBytes)
        .arg(samplingRateHz, 0, 'g', 17)
        .arg(samplingRateHz / kTdmPhaseCount, 0, 'g', 17)
        .arg(kTdmPhaseCount);
}

}  // namespace

MatlabChannelExportResult MatlabChannelExporter::exportAdcBin(const QString &adcFile,
                                                              const QString &outputDir,
                                                              double samplingRateHz)
{
    MatlabChannelExportOptions options;
    return exportAdcBin(adcFile, outputDir, samplingRateHz, options);
}

MatlabChannelExportResult MatlabChannelExporter::exportAdcBin(const QString &adcFile,
                                                              const QString &outputDir,
                                                              double samplingRateHz,
                                                              const MatlabChannelExportOptions &options)
{
    const auto cancelled = [&options]() {
        return options.cancelFlag && options.cancelFlag->load();
    };
    if (cancelled()) {
        return makeError(QStringLiteral("导出已取消"), outputDir);
    }
    SessionInput inputParts;
    QString inputError;
    if (!SessionManifest::resolveInput(adcFile, samplingRateHz, &inputParts, &inputError)) {
        return makeError(inputError, outputDir);
    }
    MultiPartInput input;
    if (!input.open(inputParts.parts, &inputError)) {
        return makeError(inputError, outputDir);
    }

    QDir out(outputDir);
    if (!out.exists() && !QDir().mkpath(outputDir)) {
        return makeError(QStringLiteral("无法创建导出目录: %1").arg(outputDir), outputDir);
    }

    const qint64 totalFrames = inputParts.totalFrames;
    const qint64 leftoverBytes = inputParts.ignoredTailBytes;
    if (totalFrames <= 0) {
        return makeError(QStringLiteral("ADC_DATA.bin 中没有完整帧"), outputDir);
    }

    if (options.mode == MatlabExportMode::TdmDemux) {
        std::vector<TdmOutput> outputs;
        outputs.reserve(kChannelsTotal * kTdmPhaseCount);
        std::vector<int> outputIndex(kChannelsTotal * kTdmPhaseCount, -1);
        for (int ch = 0; ch < kChannelsTotal; ++ch) {
            for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
                TdmOutput output;
                output.sourceChannel = ch;
                output.localEle = phase;
                output.globalEle = globalEleForChannelLocalEle(ch, phase);
                output.phase = phase;
                output.fileName = electrodeFileName(output.globalEle);
                outputIndex[ch * kTdmPhaseCount + phase] = static_cast<int>(outputs.size());
                outputs.push_back(output);
            }
        }

        std::vector<std::unique_ptr<QFile>> outputFiles;
        outputFiles.reserve(outputs.size());
        for (const TdmOutput &output : outputs) {
            if (cancelled()) {
                return makeError(QStringLiteral("导出已取消"), outputDir);
            }
            auto file = std::make_unique<QFile>(out.filePath(output.fileName));
            if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                return makeError(QStringLiteral("无法创建TDM电极文件 %1: %2")
                                     .arg(file->fileName(), file->errorString()),
                                 outputDir);
            }
            outputFiles.push_back(std::move(file));
        }

        std::vector<QByteArray> outputBuffers(outputs.size());
        std::array<std::unique_ptr<QFile>, kTdmPhaseCount> indexFiles;
        std::array<QByteArray, kTdmPhaseCount> indexBuffers;
        for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
            indexFiles[phase] = std::make_unique<QFile>(out.filePath(
                QStringLiteral("tdm_phase%1_frames_i64le.bin").arg(phase)));
            if (!indexFiles[phase]->open(QIODevice::WriteOnly | QIODevice::Truncate))
                return makeError(QStringLiteral("无法创建 TDM 时间索引文件"), outputDir);
        }
        const int chunkBytes = kFramesPerChunk * kFrameBytes;
        for (QByteArray &buffer : outputBuffers) {
            buffer.reserve(((kFramesPerChunk + kTdmPhaseCount - 1) / kTdmPhaseCount) * kBytesPerPoint);
        }

        QByteArray carry;
        qint64 exportedFrames = 0;
        // Electrode identity is the true hardware frame number modulo four.
        // The reconciler recovers that from the per-frame timestamp, so an
        // upstream frame loss captured mid-file does not rotate the phases.
        // On files whose timestamp is absent
        // (older firmware) it stays dormant and the index is just the frame
        // count.
        FrameTimestampReconciler tsRecon;
        tsRecon.reset(inputParts.metadata.frameOrigin);
        while (!input.atEnd()) {
            if (cancelled()) {
                return makeError(QStringLiteral("导出已取消"), outputDir);
            }
            QByteArray chunk = input.read(chunkBytes);
            if (chunk.isEmpty()) {
                break;
            }
            if (!carry.isEmpty()) {
                chunk.prepend(carry);
                carry.clear();
            }

            const int usable = chunk.size() - (chunk.size() % kFrameBytes);
            if (usable <= 0) {
                carry = chunk;
                continue;
            }

            const int frameCount = usable / kFrameBytes;
            for (QByteArray &buffer : outputBuffers) {
                buffer.clear();
            }
            for (auto &buffer : indexBuffers) buffer.clear();

            const char *base = chunk.constData();
            for (int f = 0; f < frameCount; ++f) {
                if ((f & 63) == 0 && cancelled()) {
                    return makeError(QStringLiteral("导出已取消"), outputDir);
                }
                const char *frame = base + f * kFrameBytes;
                // The timestamp is shared by all 256 points of a frame; read
                // it from channel 0. The reconciled index preserves phase over
                // upstream drops and falls back to file-frame order otherwise.
                const quint32 word0 = qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar *>(frame));
                const quint32 ts = (word0 >> kTimestampShift) & 0xFFFFFu;
                const qint64 sourceFrame = tsRecon.advance(ts);
                const int phase = tdmPhaseForFrame(sourceFrame);
                char indexLe[sizeof(qint64)];
                qToLittleEndian<qint64>(sourceFrame, reinterpret_cast<uchar *>(indexLe));
                indexBuffers[phase].append(indexLe, sizeof(indexLe));
                for (int ch = 0; ch < kChannelsTotal; ++ch) {
                    const int destination = outputIndex[ch * kTdmPhaseCount + phase];
                    // Each point is {timestamp[31:12], adc[11:0]}; export only the
                    // 12-bit ADC sample (little-endian int32), stripping the timestamp.
                    const uchar *p = reinterpret_cast<const uchar *>(frame + ch * kBytesPerPoint);
                    const quint32 adc =
                        (static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8)) & kAdcSampleMask;
                    const char le[4] = {
                        static_cast<char>(adc & 0xFF),
                        static_cast<char>((adc >> 8) & 0xFF),
                        0, 0};
                    outputBuffers[destination].append(le, sizeof(le));
                }
            }

            for (int i = 0; i < static_cast<int>(outputFiles.size()); ++i) {
                const QByteArray &buffer = outputBuffers[i];
                if (buffer.isEmpty()) {
                    continue;
                }
                if (outputFiles[i]->write(buffer) != buffer.size()) {
                    return makeError(QStringLiteral("写入TDM电极文件失败 %1: %2")
                                         .arg(outputFiles[i]->fileName(), outputFiles[i]->errorString()),
                                     outputDir);
                }
            }

            for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
                if (indexFiles[phase]->write(indexBuffers[phase]) != indexBuffers[phase].size())
                    return makeError(QStringLiteral("写入 TDM 时间索引失败"), outputDir);
            }
            exportedFrames += frameCount;
            carry = chunk.mid(usable);
        }

        for (auto &file : outputFiles) {
            if (!file->flush()) return makeError(QStringLiteral("TDM 电极文件写盘失败: %1").arg(file->errorString()), outputDir);
            file->close();
        }
        for (auto &file : indexFiles) {
            if (!file->flush()) return makeError(QStringLiteral("TDM 时间索引写盘失败"), outputDir);
            file->close();
        }
        if (!input.errorString().isEmpty()) {
            return makeError(QStringLiteral("读取 Session 分片失败: %1").arg(input.errorString()),
                             outputDir);
        }
        input.close();

        QString error;
        if (!writeTextFile(out.filePath(QStringLiteral("read_tdm_timeline.m")),
                           matlabTdmTimelineScript(samplingRateHz, inputParts.metadata.frameOrigin), &error))
            return makeError(QStringLiteral("写入 TDM 时间加载脚本失败: %1").arg(error), outputDir);
        if (!writeTextFile(out.filePath(QStringLiteral("load_tdm_ele_channels.m")),
                           matlabTdmLoaderScript(samplingRateHz),
                           &error)) {
            return makeError(QStringLiteral("写入 load_tdm_ele_channels.m 失败: %1").arg(error), outputDir);
        }
        if (!writeTextFile(out.filePath(QStringLiteral("read_tdm_ele.m")),
                           matlabTdmSingleEleScript(samplingRateHz),
                           &error)) {
            return makeError(QStringLiteral("写入 read_tdm_ele.m 失败: %1").arg(error), outputDir);
        }
        if (!writeTextFile(out.filePath(QStringLiteral("tdm_map.tsv")), tdmMapText(outputs), &error)) {
            return makeError(QStringLiteral("写入 tdm_map.tsv 失败: %1").arg(error), outputDir);
        }
        if (!writeTextFile(out.filePath(QStringLiteral("metadata.txt")),
                           metadataTdmText(adcFile,
                                           exportedFrames,
                                           leftoverBytes,
                                           samplingRateHz,
                                            static_cast<int>(outputs.size())) +
                                QStringLiteral("frame_index_origin=%1\nupstream_missing_frames=%2\ntime_index_pattern=tdm_phase#_frames_i64le.bin\ntime_index_type=int64 little-endian\n")
                                    .arg(inputParts.metadata.frameOrigin).arg(tsRecon.totalGap()),
                           &error)) {
            return makeError(QStringLiteral("写入 metadata.txt 失败: %1").arg(error), outputDir);
        }

        MatlabChannelExportResult result;
        result.ok = true;
        result.warning = inputParts.warning;
        if (tsRecon.totalGap() > 0)
            result.warning += QStringLiteral(" 检测到 %1 帧缺失，请使用导出的时间索引").arg(tsRecon.totalGap());
        result.outputDir = outputDir;
        result.frameCount = exportedFrames;
        result.leftoverBytes = leftoverBytes;
        result.outputCount = static_cast<int>(outputs.size());
        return result;
    }

    const bool rawWords = options.mode == MatlabExportMode::RawWords;
    std::vector<std::unique_ptr<QFile>> channelFiles;
    channelFiles.reserve(kChannelsTotal);
    for (int ch = 0; ch < kChannelsTotal; ++ch) {
        if (cancelled()) {
            return makeError(QStringLiteral("导出已取消"), outputDir);
        }
        auto file = std::make_unique<QFile>(out.filePath(channelFileName(ch, rawWords)));
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return makeError(QStringLiteral("无法创建通道文件 %1: %2")
                                 .arg(file->fileName(), file->errorString()),
                             outputDir);
        }
        channelFiles.push_back(std::move(file));
    }

    std::vector<QByteArray> channelBuffers(kChannelsTotal);
    const int chunkBytes = kFramesPerChunk * kFrameBytes;
    for (QByteArray &buffer : channelBuffers) {
        buffer.reserve(kFramesPerChunk * kBytesPerPoint);
    }

    QByteArray carry;
    qint64 exportedFrames = 0;
    while (!input.atEnd()) {
        if (cancelled()) {
            return makeError(QStringLiteral("导出已取消"), outputDir);
        }
        QByteArray chunk = input.read(chunkBytes);
        if (chunk.isEmpty()) {
            break;
        }
        if (!carry.isEmpty()) {
            chunk.prepend(carry);
            carry.clear();
        }

        const int usable = chunk.size() - (chunk.size() % kFrameBytes);
        if (usable <= 0) {
            carry = chunk;
            continue;
        }

        const int frameCount = usable / kFrameBytes;
        for (QByteArray &buffer : channelBuffers) {
            buffer.clear();
        }

        const char *base = chunk.constData();
        for (int f = 0; f < frameCount; ++f) {
            if ((f & 63) == 0 && cancelled()) {
                return makeError(QStringLiteral("导出已取消"), outputDir);
            }
            const char *frame = base + f * kFrameBytes;
            for (int ch = 0; ch < kChannelsTotal; ++ch) {
                const char *point = frame + ch * kBytesPerPoint;
                if (rawWords) {
                    channelBuffers[ch].append(point, kBytesPerPoint);
                } else {
                    const quint32 raw = qFromLittleEndian<quint32>(
                        reinterpret_cast<const uchar *>(point));
                    const quint32 adc = raw & kAdcSampleMask;
                    const char le[4] = {
                        static_cast<char>(adc & 0xFF),
                        static_cast<char>((adc >> 8) & 0xFF),
                        0,
                        0};
                    channelBuffers[ch].append(le, sizeof(le));
                }
            }
        }

        for (int ch = 0; ch < kChannelsTotal; ++ch) {
            const QByteArray &buffer = channelBuffers[ch];
            if (channelFiles[ch]->write(buffer) != buffer.size()) {
                return makeError(QStringLiteral("写入通道文件失败 %1: %2")
                                     .arg(channelFiles[ch]->fileName(), channelFiles[ch]->errorString()),
                                 outputDir);
            }
        }

        exportedFrames += frameCount;
        carry = chunk.mid(usable);
    }

    for (auto &file : channelFiles) {
        if (!file->flush()) return makeError(QStringLiteral("通道文件写盘失败: %1").arg(file->errorString()), outputDir);
        file->close();
    }
    if (!input.errorString().isEmpty()) {
        return makeError(QStringLiteral("读取 Session 分片失败: %1").arg(input.errorString()),
                         outputDir);
    }
    input.close();

    QString error;
    if (!writeTextFile(out.filePath(QStringLiteral("load_adc_channels.m")),
                       matlabLoaderScript(exportedFrames, samplingRateHz),
                       &error)) {
        return makeError(QStringLiteral("写入 load_adc_channels.m 失败: %1").arg(error), outputDir);
    }
    if (!writeTextFile(out.filePath(QStringLiteral("read_adc_channel.m")),
                       matlabSingleChannelScript(exportedFrames, samplingRateHz, rawWords),
                       &error)) {
        return makeError(QStringLiteral("写入 read_adc_channel.m 失败: %1").arg(error), outputDir);
    }
    if (!writeTextFile(out.filePath(QStringLiteral("metadata.txt")),
                       metadataText(adcFile,
                                    exportedFrames,
                                    leftoverBytes,
                                    samplingRateHz,
                                    rawWords),
                       &error)) {
        return makeError(QStringLiteral("写入 metadata.txt 失败: %1").arg(error), outputDir);
    }

    MatlabChannelExportResult result;
    result.ok = true;
    result.warning = inputParts.warning;
    result.outputDir = outputDir;
    result.frameCount = exportedFrames;
    result.leftoverBytes = leftoverBytes;
    result.outputCount = kChannelsTotal;
    return result;
}

}  // namespace ccv2
