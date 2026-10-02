// Proves the timestamp-driven fix: when frames are lost
// upstream mid-capture, the exported TDM electrode files still carry the right
// electrode's samples — because phase is taken from each frame's hardware
// timestamp, not its position in the (now gapped) file.

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
#include <vector>

using namespace ccv2;

namespace {
// One frame carrying hardware frame number `trueFrame` in BOTH the timestamp
// (high 20 bits, all channels) and channel 5's ADC value (low 12 bits), so the
// electrode file contents are self-identifying.
QByteArray frameFor(int trueFrame)
{
    QByteArray fr(kFrameBytes, Qt::Uninitialized);
    const quint32 raw = (static_cast<quint32>(trueFrame & 0xFFFFF) << kTimestampShift) |
                        static_cast<quint32>(trueFrame & kAdcSampleMask);
    for (int ch = 0; ch < kChannelsTotal; ++ch) {
        qToLittleEndian<quint32>(raw, reinterpret_cast<uchar *>(fr.data() + ch * kBytesPerPoint));
    }
    return fr;
}

qint32 sampleAt(const QByteArray &b, int i)
{
    return qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(b.constData() + i * kBytesPerPoint));
}
}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) { std::cerr << "temp dir failed" << std::endl; return 1; }

    // True frames 0..9 with frame 3 LOST upstream (odd gap). The recorded file
    // is therefore a dense run of {0,1,2,4,5,6,7,8,9} whose position parity
    // diverges from the true parity after the gap.
    const std::vector<int> present = {0, 1, 2, 4, 5, 6, 7, 8, 9};
    QByteArray frames;
    for (int t : present) frames += frameFor(t);

    const QString adcFile = QDir(dir.path()).filePath(QStringLiteral("ADC.bin"));
    {
        QFile f(adcFile);
        if (!f.open(QIODevice::WriteOnly) || f.write(frames) != frames.size()) {
            std::cerr << "write ADC failed" << std::endl; return 2;
        }
    }

    const QString outDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    MatlabChannelExportOptions opts;
    opts.mode = MatlabExportMode::TdmDemux;
    const MatlabChannelExportResult res =
        MatlabChannelExporter::exportAdcBin(adcFile, outDir, 20000.0, opts);
    if (!res.ok) {
        std::cerr << "export failed: " << res.error.toStdString() << std::endl; return 3;
    }

    QFile ele20(QDir(outDir).filePath(QStringLiteral("ele0020_i32le.bin")));
    QFile ele21(QDir(outDir).filePath(QStringLiteral("ele0021_i32le.bin")));
    QFile ele22(QDir(outDir).filePath(QStringLiteral("ele0022_i32le.bin")));
    QFile ele23(QDir(outDir).filePath(QStringLiteral("ele0023_i32le.bin")));
    if (!ele20.open(QIODevice::ReadOnly) || !ele21.open(QIODevice::ReadOnly) ||
        !ele22.open(QIODevice::ReadOnly) || !ele23.open(QIODevice::ReadOnly)) {
        std::cerr << "electrode files missing" << std::endl; return 4;
    }
    const QByteArray phaseBytes[] = {ele20.readAll(), ele21.readAll(),
                                     ele22.readAll(), ele23.readAll()};

    const std::vector<int> expected[] = {{0, 4, 8}, {1, 5, 9}, {2, 6}, {7}};
    for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
        if (phaseBytes[phase].size() !=
            static_cast<int>(expected[phase].size()) * kBytesPerPoint) {
            std::cerr << "phase file size wrong after gap" << std::endl;
            return 5;
        }
        for (size_t i = 0; i < expected[phase].size(); ++i) {
            if (sampleAt(phaseBytes[phase], static_cast<int>(i)) != expected[phase][i]) {
                std::cerr << "phase " << phase << " rotated across gap at " << i << std::endl;
                return 6;
            }
        }
    }

    std::cout << "tdm_timestamp_gap_export_smoke ok" << std::endl;
    return 0;
}
