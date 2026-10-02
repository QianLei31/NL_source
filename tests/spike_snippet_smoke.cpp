#include "service/spike_detect_worker.h"
#include "signal/spike_snippet_store.h"

#include <QByteArray>
#include <QVector>
#include <QtEndian>

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>

#include "core/constants.h"

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

// One 256-channel frame: `values` are volts, packed as 12-bit ADC codes with
// the 20-bit frame counter in the high bits (the real stream's layout).
void appendFrame(QByteArray *out, const QVector<double> &values, quint32 frameIndex)
{
    for (int ch = 0; ch < ccv2::kChannelsTotal; ++ch) {
        const double v = values.value(ch, 0.9);
        const int code = std::max(0, std::min(4095, static_cast<int>(std::lround(v / 1.8 * 4096.0))));
        const quint32 word = (static_cast<quint32>(frameIndex & 0xFFFFFu) << ccv2::kTimestampShift) |
                             static_cast<quint32>(code);
        char buf[4];
        qToLittleEndian<quint32>(word, buf);
        out->append(buf, 4);
    }
}

}  // namespace

int main()
{
    // --- store: ring semantics + incremental fetch -------------------------
    {
        ccv2::SpikeSnippetStore store;
        store.configure(2, 4, 3);  // 2 channels, 4 samples/snippet, keep 3

        QVector<float> flat;
        quint64 seq = 0;
        check(store.fetchNew(0, &seq, &flat) == 0, "empty store yields nothing");

        for (int s = 0; s < 5; ++s) {
            const float snip[4] = {float(s), float(s) + 0.1f, float(s) + 0.2f, float(s) + 0.3f};
            store.addSnippet(0, snip);
        }
        check(store.totalSpikes(0) == 5, "total counts every spike");

        quint64 limitedSeq = 0;
        const int limited = store.fetchNew(0, &limitedSeq, &flat, 2);
        check(limited == 2, "limited fetch enforces the display budget");
        check(limitedSeq == 5, "limited fetch advances past skipped snippets");
        check(std::abs(flat[0] - 3.0f) < 1e-6, "limited fetch keeps the newest subset");

        seq = 0;
        const int got = store.fetchNew(0, &seq, &flat);
        check(got == 3, "fetchNew clamps a far-behind consumer to the capacity");
        // Oldest-first, and the two oldest snippets were evicted.
        check(std::abs(flat[0] - 2.0f) < 1e-6, "ring keeps the newest snippets, oldest-first");
        check(std::abs(flat[static_cast<qsizetype>(2) * 4] - 4.0f) < 1e-6, "last fetched is the newest");
        check(store.fetchNew(0, &seq, &flat) == 0, "a caught-up consumer gets nothing");

        const float extra[4] = {9.0f, 9.1f, 9.2f, 9.3f};
        store.addSnippet(0, extra);
        check(store.fetchNew(0, &seq, &flat) == 1, "only the new snippet is returned");
        check(std::abs(flat[0] - 9.0f) < 1e-6, "incremental fetch returns the right one");

        quint64 allSeq = 0;
        check(store.fetchAll(0, &flat, &allSeq) == 3, "fetchAll returns the retained set");
        check(store.fetchNew(1, &seq, &flat) == 0, "channels are independent");

        store.clear();
        quint64 afterClear = 0;
        check(store.fetchAll(0, &flat, &afterClear) == 0, "clear empties the ring");
        check(store.totalSpikes(0) == 0, "clear resets the counter");
    }

    // --- worker: detects spikes and aligns snippets on the crossing --------
    {
        constexpr double kFs = 20000.0;
        constexpr int kPre = 8;
        constexpr int kPost = 24;
        constexpr int kSpikeFrame = 4000;   // well past the RMS warm-up
        constexpr int kTotalFrames = 12000;
        constexpr int kTestChannel = 7;

        // Big enough for the whole test feed, and pushed without dropping, so
        // the result never depends on how fast the worker drains.
        auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(512);
        ccv2::SpikeSnippetStore store;
        store.configure(ccv2::kChannelsTotal, kPre + kPost + 1, 16);

        ccv2::SpikeDetectConfig cfg;
        cfg.proc.sampleRate = kFs;
        cfg.proc.notchHz = 0;
        cfg.proc.highpassHz = 250.0;
        cfg.proc.absoluteThreshold = true;
        cfg.proc.absThresholdV = -100e-6;  // negative-going, 100 uV
        cfg.proc.negativePolarity = true;
        cfg.inputGain = 60.0;
        cfg.preSamples = kPre;
        cfg.postSamples = kPost;

        std::atomic_bool stop{false};
        ccv2::SpikeDetectWorker worker(queue, &store, &stop, cfg);
        check(worker.snippetLength() == kPre + kPost + 1, "snippet length = pre + post + 1");
        worker.start();

        // Baseline plus one clean negative deflection on a single channel.
        QVector<double> values(ccv2::kChannelsTotal, 0.9);
        QByteArray chunk;
        for (int f = 0; f < kTotalFrames; ++f) {
            values.fill(0.9);
            const int rel = f - kSpikeFrame;
            if (rel >= 0 && rel < 12) {
                // 700 uV at the input appears as 42 mV at the ADC for 60x
                // front-end gain, then the worker converts it back before
                // filtering, detection, and snippet storage.
                values[kTestChannel] =
                    0.9 - (700e-6 * cfg.inputGain) *
                              std::sin(M_PI * rel / 12.0);
            }
            appendFrame(&chunk, values, static_cast<quint32>(f));
            if (chunk.size() >= 64 * ccv2::kFrameBytes) {
                while (!queue->push(chunk, false)) {
                    QThread::msleep(5);  // never drop: the spike may be in here
                }
                chunk.clear();
            }
        }
        while (!chunk.isEmpty() && !queue->push(chunk, false)) {
            QThread::msleep(5);
        }

        // Wait for the queue to drain, then give the worker a moment to finish
        // the last chunk before reading its results.
        for (int i = 0; i < 400 && queue->size() > 0; ++i) {
            QThread::msleep(5);
        }
        check(queue->size() == 0, "worker drains the queue");
        QThread::msleep(100);
        stop.store(true);
        queue->wakeAll();
        check(worker.wait(5000), "worker exits on its stop flag");

        const qint64 detected = store.totalSpikes(kTestChannel);
        check(detected >= 1, "the injected spike is detected");
        check(detected <= 3, "the refractory period prevents a burst of retriggers");

        QVector<float> flat;
        quint64 seq = 0;
        const int have = store.fetchAll(kTestChannel, &flat, &seq);
        check(have >= 1, "a snippet was stored for the detected spike");
        if (have >= 1) {
            const int len = kPre + kPost + 1;
            // The alignment contract: the minimum of a negative-going snippet
            // sits at (or just after) the pre-sample mark, never at the edges.
            int argMin = 0;
            for (int k = 1; k < len; ++k) {
                if (flat[k] < flat[argMin]) argMin = k;
            }
            check(argMin >= kPre - 2 && argMin <= kPre + 6,
                  "snippet trough lands at the threshold crossing");
            check(flat[argMin] < -50e-6f, "snippet carries the spike's amplitude");
            check(std::abs(flat[argMin]) < 2e-3f,
                  "snippet is input-referred, not ADC-output voltage");
            check(std::abs(flat[0]) < std::abs(flat[argMin]),
                  "the pre-trigger baseline is quieter than the trough");
        }

        // Untouched channels must stay silent on a clean baseline.
        check(store.totalSpikes(kTestChannel + 1) == 0, "quiet channels do not fire");
    }

    if (failures == 0) {
        std::cout << "spike snippet smoke OK\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
