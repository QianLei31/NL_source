// Covers four-phase TDM export. The 0/2 versus 1/3 display choice must not alter
// offline export: all four physical phases are always written.

#include "core/constants.h"
#include "core/channel_routing.h"
#include "io/matlab_channel_exporter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <iostream>

using namespace ccv2;

namespace {
constexpr int kFrames = 512;

// Stamp each frame's channel-5 ADC value with the frame index so all four
// frames are individually identifiable in the exported electrode files.
QByteArray makeFrames()
{
    QByteArray frames(kFrames * kFrameBytes, Qt::Uninitialized);
    for (int f = 0; f < kFrames; ++f) {
        for (int ch = 0; ch < kChannelsTotal; ++ch) {
            const quint32 adc = static_cast<quint32>(f & kAdcSampleMask);
            char *dst = frames.data() + f * kFrameBytes + ch * kBytesPerPoint;
            qToLittleEndian<quint32>(adc, reinterpret_cast<uchar *>(dst));
        }
    }
    return frames;
}

qint32 sampleAt(const QByteArray &bytes, int index)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + index * kBytesPerPoint));
}
}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "temp dir failed" << std::endl;
        return 1;
    }

    const QByteArray frames = makeFrames();
    const QString adcFile = QDir(dir.path()).filePath(QStringLiteral("ADC.bin"));
    {
        QFile f(adcFile);
        if (!f.open(QIODevice::WriteOnly) || f.write(frames) != frames.size()) {
            std::cerr << "write ADC failed" << std::endl;
            return 2;
        }
    }

    const QString outDir = QDir(dir.path()).filePath(QStringLiteral("out_phase_b"));
    MatlabChannelExportOptions opts;
    opts.mode = MatlabExportMode::TdmDemux;
    opts.evenSampleIsFirstLocalEle = false;   // legacy display choice, ignored by export
    const MatlabChannelExportResult res =
        MatlabChannelExporter::exportAdcBin(adcFile, outDir, 20000.0, opts);
    if (!res.ok || res.frameCount != kFrames || res.outputCount != 1024) {
        std::cerr << "four-phase export failed: " << res.error.toStdString() << std::endl;
        return 3;
    }

    QFile ele20(QDir(outDir).filePath(QStringLiteral("ele0020_i32le.bin")));
    QFile ele21(QDir(outDir).filePath(QStringLiteral("ele0021_i32le.bin")));
    QFile ele22(QDir(outDir).filePath(QStringLiteral("ele0022_i32le.bin")));
    QFile ele23(QDir(outDir).filePath(QStringLiteral("ele0023_i32le.bin")));
    if (!ele20.open(QIODevice::ReadOnly) || !ele21.open(QIODevice::ReadOnly) ||
        !ele22.open(QIODevice::ReadOnly) || !ele23.open(QIODevice::ReadOnly)) {
        std::cerr << "electrode files missing" << std::endl;
        return 4;
    }
    const QByteArray phaseBytes[] = {ele20.readAll(), ele21.readAll(),
                                     ele22.readAll(), ele23.readAll()};
    const int tdmSamples = kFrames / kTdmPhaseCount;
    for (const QByteArray &bytes : phaseBytes) {
        if (bytes.size() != tdmSamples * kBytesPerPoint) {
        std::cerr << "electrode file size wrong" << std::endl;
        return 5;
        }
    }
    for (int s : {0, 1, tdmSamples - 1}) {
        for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
            if (sampleAt(phaseBytes[phase], s) != kTdmPhaseCount * s + phase) {
                std::cerr << "phase " << phase << " mis-routed at " << s << std::endl;
                return 6;
            }
        }
    }

    std::cout << "tdm_phase_b_export_smoke ok" << std::endl;
    return 0;
}
