// P2 coverage: with a software common-average reference enabled, the heatmap
// "mean" metric must report the RAW DC level (so the 0..1.8 V colour scale is
// meaningful), while RMS stays computed on the reference-subtracted signal.

#include "core/constants.h"
#include "network/data_sorter.h"
#include "core/threadsafe_queue.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>

using namespace ccv2;

namespace {
constexpr int kFrames = 200;

// Constant DC: channel 0 sits at codeA, every other channel at codeB. The
// common-average reference is therefore a constant, so channel 0's referenced
// signal is constant (RMS 0) while its raw mean stays at codeA.
QByteArray makeFrames(quint32 codeA, quint32 codeB)
{
    QByteArray frames(kFrames * kFrameBytes, Qt::Uninitialized);
    for (int f = 0; f < kFrames; ++f) {
        for (int ch = 0; ch < kChannelsTotal; ++ch) {
            const quint32 adc = (ch == 0 ? codeA : codeB) & kAdcSampleMask;
            char *dst = frames.data() + f * kFrameBytes + ch * kBytesPerPoint;
            qToLittleEndian<quint32>(adc, reinterpret_cast<uchar *>(dst));
        }
    }
    return frames;
}

bool waitFor(const std::function<bool()> &pred, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (pred()) return true;
        QThread::msleep(20);
    }
    return pred();
}
}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const quint32 codeA = 3000;
    const quint32 codeB = 1000;
    const QByteArray frames = makeFrames(codeA, codeB);
    const double rawMeanVolts =
        static_cast<double>(codeA) / 4096.0 * 1.8;   // expected metric.mean

    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(8);
    auto state = std::make_shared<RealtimeStreamState>();
    std::atomic_bool stopFlag{false};
    std::atomic<int> refMode{1};   // 1 = common-average reference (CAR)
    {
        QMutexLocker locker(&state->lock);
        state->channels = {0};
        state->metricWindowFrames = 100;
    }

    DataSorter sorter(queue, state, &stopFlag,
                      /*timelineEpoch=*/nullptr, &refMode);
    sorter.start();
    queue->push(frames, true);

    const bool ok = waitFor([&]() {
        QMutexLocker locker(&state->lock);
        return state->metrics.size() == kChannelsTotal && state->metrics[0].valid;
    }, 3000);
    stopFlag.store(true);
    queue->wakeAll();
    sorter.wait(1000);

    if (!ok) {
        std::cerr << "metrics did not publish" << std::endl;
        return 1;
    }

    QMutexLocker locker(&state->lock);
    const RealtimeChannelMetric m = state->metrics[0];
    if (std::abs(m.mean - rawMeanVolts) > 1e-9) {
        std::cerr << "mean should be the RAW DC level " << rawMeanVolts
                  << " but was " << m.mean << std::endl;
        return 2;
    }
    if (m.rms > 1e-9) {
        std::cerr << "RMS of a constant referenced signal should be ~0, was "
                  << m.rms << std::endl;
        return 3;
    }

    std::cout << "mean_metric_reference_smoke ok mean=" << m.mean << std::endl;
    return 0;
}
