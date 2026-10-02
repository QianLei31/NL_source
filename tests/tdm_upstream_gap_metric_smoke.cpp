// Proves the live self-heal: when frames are lost upstream
// (a timestamp jump), DataSorter keeps binning each electrode's TDM heatmap
// metric into the correct four-phase slot.

#include "core/constants.h"
#include "core/channel_routing.h"
#include "core/threadsafe_queue.h"
#include "network/data_sorter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

using namespace ccv2;

namespace {
QByteArray frameFor(int t, const quint32 codes[4])
{
    QByteArray fr(kFrameBytes, Qt::Uninitialized);
    for (int ch = 0; ch < kChannelsTotal; ++ch) {
        const quint32 adc = codes[t % kTdmPhaseCount] & kAdcSampleMask;
        const quint32 raw =
            (static_cast<quint32>(t & 0xFFFFF) << kTimestampShift) | adc;
        qToLittleEndian<quint32>(raw, reinterpret_cast<uchar *>(fr.data() + ch * kBytesPerPoint));
    }
    return fr;
}

bool waitFor(const std::function<bool()> &pred, int timeoutMs)
{
    for (int w = 0; w < timeoutMs; w += 20) {
        if (pred()) return true;
        QThread::msleep(20);
    }
    return pred();
}
}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const quint32 codes[4] = {700, 1400, 2300, 3300};
    double volts[4]{};
    for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
        volts[phase] = static_cast<double>(codes[phase]) / 4096.0 * 1.8;
    }

    // True frames 0..7 with frame 3 lost upstream (odd gap). The naive frame
    // count would put every post-gap frame in the wrong slot.
    const std::vector<int> present = {0, 1, 2, 4, 5, 6, 7};
    QByteArray frames;
    for (int t : present) frames += frameFor(t, codes);

    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(8);
    auto state = std::make_shared<RealtimeStreamState>();
    std::atomic_bool stopFlag{false};
    {
        QMutexLocker locker(&state->lock);
        state->channels = {0};
        state->metricWindowFrames = 4;   // publish after 4 frames
    }

    DataSorter sorter(queue, state, &stopFlag);
    sorter.start();
    queue->push(frames, true);           // one contiguous chunk (no in-app drop)

    const bool ok = waitFor([&]() {
        QMutexLocker locker(&state->lock);
        return state->tdmMetrics.size() == kChannelsTotal * kTdmPhaseCount &&
               state->tdmMetrics[0].valid && state->tdmMetrics[1].valid &&
               state->tdmMetrics[2].valid && state->tdmMetrics[3].valid;
    }, 3000);
    stopFlag.store(true);
    queue->wakeAll();
    sorter.wait(1000);

    if (!ok) {
        std::cerr << "TDM metrics did not publish" << std::endl;
        return 1;
    }

    QMutexLocker locker(&state->lock);
    for (int phase = 0; phase < kTdmPhaseCount; ++phase) {
        if (std::abs(state->tdmMetrics[phase].mean - volts[phase]) > 1e-9) {
            std::cerr << "phase " << phase << " mislabelled across gap: "
                      << state->tdmMetrics[phase].mean << " vs " << volts[phase] << std::endl;
            return 2;
        }
    }

    std::cout << "tdm_upstream_gap_metric_smoke ok" << std::endl;
    return 0;
}
