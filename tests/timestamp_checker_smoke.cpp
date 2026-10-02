#include "core/constants.h"
#include "network/timestamp_checker.h"

#include <QByteArray>
#include <QtEndian>

#include <iostream>

namespace {

constexpr quint32 kTimestampMask = (1u << 20) - 1u;

QByteArray makeFrames(const QVector<quint32> &timestamps,
                      int mismatchedFrame = -1,
                      int mismatchedChannel = -1)
{
    QByteArray bytes(timestamps.size() * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int frame = 0; frame < timestamps.size(); ++frame) {
        for (int channel = 0; channel < ccv2::kChannelsTotal; ++channel) {
            quint32 timestamp = timestamps[frame] & kTimestampMask;
            if (frame == mismatchedFrame && channel == mismatchedChannel) {
                timestamp = (timestamp + 7u) & kTimestampMask;
            }
            const quint32 adc = static_cast<quint32>(
                (frame * 17 + channel) & ccv2::kAdcSampleMask);
            const quint32 raw =
                (timestamp << ccv2::kTimestampShift) | adc;
            char *dst = bytes.data() +
                        frame * ccv2::kFrameBytes +
                        channel * ccv2::kBytesPerPoint;
            qToLittleEndian<quint32>(
                raw,
                reinterpret_cast<uchar *>(dst));
        }
    }
    return bytes;
}

}  // namespace

int main()
{
    {
        ccv2::TimestampContinuityAnalyzer analyzer(1);
        const QByteArray frames = makeFrames({
            kTimestampMask - 2u,
            kTimestampMask - 1u,
            kTimestampMask,
            0u,
            1u,
        });
        analyzer.process(frames.left(333));
        analyzer.process(frames.mid(333));
        const auto stats = analyzer.stats();
        if (stats.framesSeen != 5 ||
            stats.discontinuities != 0 ||
            stats.intraFrameMismatchFrames != 0 ||
            stats.firstTimestamp != kTimestampMask - 2u ||
            stats.lastTimestamp != 1u) {
            std::cerr << "20-bit timestamp wrap check failed" << std::endl;
            return 1;
        }
    }

    {
        ccv2::TimestampContinuityAnalyzer analyzer(1);
        analyzer.process(makeFrames({100u, 101u, 104u, 105u}));
        const auto stats = analyzer.stats();
        if (stats.discontinuities != 1 ||
            stats.estimatedMissingFrames != 2 ||
            stats.firstErrorFrame != 2 ||
            stats.firstErrorExpected != 102u ||
            stats.firstErrorActual != 104u) {
            std::cerr << "missing-frame estimation failed" << std::endl;
            return 2;
        }
    }

    {
        ccv2::TimestampContinuityAnalyzer analyzer(1);
        analyzer.process(makeFrames({10u, 11u, 12u}, 1, 17));
        const auto stats = analyzer.stats();
        if (stats.intraFrameMismatchFrames != 1 ||
            stats.mismatchedChannelWords != 1 ||
            stats.firstErrorFrame != 1 ||
            stats.firstErrorExpected != 11u ||
            stats.firstErrorActual != 18u) {
            std::cerr << "intra-frame timestamp mismatch check failed"
                      << std::endl;
            return 3;
        }
    }

    {
        QVector<quint32> timestamps;
        for (quint32 i = 0; i < 16; ++i) {
            timestamps.push_back((900000u + i * 256u) & kTimestampMask);
        }
        ccv2::TimestampContinuityAnalyzer analyzer(0);
        analyzer.process(makeFrames(timestamps));
        const auto stats = analyzer.stats();
        if (!stats.calibrated ||
            stats.expectedStep != 256u ||
            stats.discontinuities != 0) {
            std::cerr << "automatic timestamp step detection failed"
                      << std::endl;
            return 4;
        }
    }

    {
        ccv2::TimestampContinuityAnalyzer analyzer(0);
        analyzer.process(makeFrames({
            100u, 101u, 103u, 104u, 105u,
            106u, 107u, 108u, 109u, 110u,
        }));
        const auto stats = analyzer.stats();
        if (!stats.calibrated ||
            stats.expectedStep != 1u ||
            stats.transitionsChecked != 9 ||
            stats.discontinuities != 1 ||
            stats.estimatedMissingFrames != 1 ||
            stats.firstErrorFrame != 2) {
            std::cerr << "automatic calibration hid an early dropped frame"
                      << std::endl;
            return 5;
        }
    }

    return 0;
}
