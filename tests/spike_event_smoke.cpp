#include "service/spike_detect_worker.h"
#include "signal/spike_snippet_store.h"
#include "core/constants.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtEndian>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}

struct Result { QVector<ccv2::SpikeEvent> events; QVector<float> waves; };
// Deterministic rectangular-pulse ground truth. Threshold crossing includes
// causal filter delay, so assertions use a bounded interval after each onset.
Result detect(int chunkFrames, bool tdm = false, bool pair02 = true,
              qint64 origin = 0, bool gap = false, bool stamped = true, bool invalidFirst = false, bool unknown = false) {
    constexpr int frames = 3600, adc = 7;
    const int phase = pair02 ? 2 : 3;
    const int lane = tdm ? adc * 2 + 1 : adc;
    ccv2::SpikeDetectConfig cfg;
    cfg.proc.sampleRate = tdm ? 5000 : 20000;
    cfg.proc.notchHz = 0;
    cfg.proc.highpassHz = 250;
    cfg.proc.spikeLowpassHz = cfg.proc.sampleRate * 0.45;
    cfg.proc.absoluteThreshold = true;
    cfg.proc.absThresholdV = -100e-6;
    cfg.proc.refractoryMs = 5.0;
    cfg.inputGain = 60;
    cfg.preSamples = 8;
    cfg.postSamples = 24;
    cfg.tdmEnabled = tdm;
    cfg.tdmPair02 = pair02;
    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>();
    ccv2::SpikeSnippetStore store;
    check(store.configure(tdm ? 512 : 256, 33, 32), "event store allocation");
    store.resetTimeline(17);
    std::atomic<quint64> epoch{17};
    std::atomic_bool stop{false};
    ccv2::SpikeDetectWorker worker(queue, &store, &stop, cfg, &epoch, nullptr, origin);

    // Ground truth onsets are f=800 and f=2400 (ADC) or the first
    // target physical phase at/after those frames (TDM).
    for (int from = 0; from < frames; from += chunkFrames) {
        const int count = std::min(chunkFrames, frames - from);
        QByteArray bytes(count * ccv2::kFrameBytes, '\0');
        ccv2::StreamBlockInfo info;
        info.epoch = 17;
        info.integrityUnknown = unknown;
        for (int j = 0; j < count; ++j) {
            const int f = from + j;
            const qint64 source = origin + f + ((gap && f >= 1800) ? 104 : 0);
            if (stamped) info.frameIndices.append(source);
            if (invalidFirst) info.frameValid.append(!(f >= 800 && f < 840));
            for (int ch = 0; ch < 256; ++ch) {
                int code = 2048;
                const bool injected = (f >= 800 && f < 824) || (f >= 2400 && f < 2424);
                if (ch == adc && injected && (!tdm || source % 4 == phase)) code -= 90;
                const quint32 word = ((quint32(source) & 0xfffffu) << 12) | quint32(code);
                qToLittleEndian<quint32>(word, bytes.data() + j * ccv2::kFrameBytes + ch * 4);
            }
        }
        queue->push(bytes, false, nullptr, info);
    }
    worker.start();
    QElapsedTimer timer; timer.start();
    while (queue->size() > 0 && timer.elapsed() < 5000) QThread::msleep(5);
    check(queue->size() == 0, "event worker drains queue");
    const qint64 expectedExposure = unknown ? 0 : (frames - (invalidFirst ? 40 : 0)) / (tdm ? 4 : 1);
    QVector<qint64> exposure;
    ccv2::SpikeAnalysisQuality quality;
    do {
        store.snapshotAnalysis(&exposure, &quality);
        if (unknown ? quality.invalidFrames == frames : exposure.value(lane) == expectedExposure) break;
        QThread::msleep(5);
    } while (timer.elapsed() < 5000);
    check(exposure.value(lane) == expectedExposure, "exposure measures valid processed samples");
    check(!gap || quality.missingSourceFrames == 104, "gap quality retains missing source frames");
    check(!(invalidFirst || unknown) || quality.invalidFrames == (unknown ? frames : 40), "invalid samples explicitly accounted");
    stop.store(true); queue->wakeAll();
    check(worker.wait(5000), "event worker stops cleanly");
    Result result;
    store.fetchEvents(lane, &result.events, &result.waves);
    const int expectedEvents = unknown ? 0 : (invalidFirst ? 1 : 2);
    if (result.events.size() != expectedEvents) { std::cerr << "case chunk=" << chunkFrames << " tdm=" << tdm << " origin=" << origin << " frames:"; for (const auto &e : result.events) std::cerr << " " << e.sourceFrame; std::cerr << "\n"; }
    check(result.events.size() == expectedEvents, "only valid ground-truth events detected");
    check(store.totalSpikes(tdm ? lane - 1 : adc + 1) == 0, "quiet lane remains silent");
    for (int i = 0; i < result.events.size(); ++i) {
        const auto &e = result.events[i];
        const int truthIndex = i + (invalidFirst ? 1 : 0);
        const qint64 expected = origin + (truthIndex == 0 ? 800 : 2400) + ((gap && truthIndex == 1) ? 104 : 0);
        check(e.sourceFrame >= expected && e.sourceFrame < expected + (tdm ? 16 : 8), "source frame follows truth onset and filter delay");
        check(e.epoch == 17 && e.sequence == quint64(i + 1), "epoch and sequence preserved");
        check(e.lane == lane && e.adcChannel == adc, "lane and ADC identity preserved");
        check(e.tdmPhase == (tdm ? phase : -1), "physical TDM phase preserved");
        check(e.electrode == (tdm ? adc * 4 + phase : adc), "physical electrode preserved");
        check(e.sampleStride == (tdm ? 4 : 1) && e.sourceSampleRate == 20000, "source sample-rate/stride correct");
        check(e.inputGain == 60 && e.preSamples == 8 && e.unitId == -1, "calibration/alignment/unsorted semantics preserved");
        check(std::abs(e.sourceTimeSeconds() - double(e.sourceFrame) / 20000) < 1e-12, "source time uses source clock");
        check(result.waves[i * 33 + 8] <= -100e-6f, "waveform crossing matches timestamp");
    }
    return result;
}

void boundaryAndQueueCases() {
    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(1);
    ccv2::SpikeSnippetStore store; store.configure(256, 33, 4);
    ccv2::SpikeDetectConfig cfg; cfg.inputGain=60; cfg.proc.notchHz=0;
    cfg.proc.absoluteThreshold=true; cfg.proc.absThresholdV=-100e-6;
    cfg.proc.refractoryMs=5; cfg.preSamples=8; cfg.postSamples=24;
    QByteArray discarded(100*ccv2::kFrameBytes, '\0');
    queue->push(discarded);
    QByteArray data(128*ccv2::kFrameBytes,'\0');
    ccv2::StreamBlockInfo info; info.frameValid.fill(true,128);
    for(int f=0;f<128;++f) {
        info.frameIndices.append(100+f);
        for(int ch=0;ch<256;++ch) {
            const int code=ch==0 && ((f>=1 && f<5) || f>=125) ? 1950 : 2048;
            qToLittleEndian<quint32>((quint32(100+f)<<12)|quint32(code),data.data()+f*ccv2::kFrameBytes+ch*4);
        }
    }
    queue->push(data,true,nullptr,info);
    std::atomic_bool stop{false};
    ccv2::SpikeDetectWorker worker(queue,&store,&stop,cfg); worker.start();
    QElapsedTimer t;t.start();
    QVector<qint64> exposure; ccv2::SpikeAnalysisQuality quality;
    do {
        store.snapshotAnalysis(&exposure,&quality);
        if(exposure.value(255)==128) break;
        QThread::msleep(1);
    }while(t.elapsed()<5000);
    stop.store(true);queue->wakeAll();check(worker.wait(5000),"boundary worker stops");
    store.snapshotAnalysis(&exposure,&quality);
    check(quality.queueDroppedFrames==100 && quality.missingSourceFrames==100,"subscriber saturation explicitly counted");
    check(quality.pendingWindowEvents==1,"EOF crossing awaiting post-window remains visible");
    check(quality.boundaryExcludedEvents==1,"crossing without full pre-window explicitly excluded");
    check(store.totalSpikes(0)==0,"incomplete windows never fabricated as complete events");
}

void storeCases() {
    ccv2::SpikeSnippetStore store;
    check(store.configure(1, 4, 3), "store config");
    store.resetTimeline(4);
    for (int i = 0; i < 5; ++i) {
        const float samples[] = {float(i), -1, 0, 1};
        ccv2::SpikeEvent e; e.epoch = 4; e.sourceFrame = 100 + i;
        e.sourceSampleRate = 20000; e.adcChannel = 0; e.electrode = 0;
        check(store.addEventIfEpoch(0, samples, e), "event accepted");
    }
    QVector<ccv2::SpikeEvent> events; QVector<float> waves; quint64 seq = 0;
    check(store.fetchEvents(0, &events, &waves, &seq) == 3 && seq == 5, "bounded event snapshot");
    for (int i = 0; i < events.size(); ++i) {
        check(events[i].sourceFrame == 102 + i && events[i].sequence == quint64(3 + i), "ring metadata rollover");
        check(waves[i * 4] == float(i + 2), "metadata and waveform remain paired");
    }
    const float sample[4] = {};
    ccv2::SpikeEvent bad = events.first(); bad.epoch = 3;
    check(!store.addEventIfEpoch(0, sample, bad), "stale epoch rejected");
    bad.epoch = 4; bad.sourceSampleRate = std::numeric_limits<double>::quiet_NaN();
    check(!store.addEventIfEpoch(0, sample, bad), "invalid rate rejected");
    bad = events.first();
    check(!store.addEventIfEpoch(1, sample, bad), "invalid channel rejected");
    check(!store.addSnippetIfEpoch(1, sample, 4), "legacy invalid channel truthfully rejected");
    store.resetTimeline(5);
    check(store.fetchEvents(0, &events) == 0, "timeline reset clears event metadata");
    store.addSnippet(0, sample);
    store.fetchEvents(0, &events);
    check(events.size() == 1 && !events[0].hasSourceTime() && events[0].epoch == 5, "legacy producer has explicit unknown timestamp");
    const quint64 oldSequence = events[0].sequence;
    store.configure(1, 4, 3);
    store.addSnippet(0, sample);
    check(store.setCandidateUnit(5, 0, {oldSequence}, 1) == 0, "configuration reset cannot alias a stale selection");
    auto snapshot = store.snapshotLane(0);
    check(snapshot.events[0].sequence > oldSequence, "sequence remains monotonic across configure");
    check(store.setCandidateUnit(5, 0, {snapshot.events[0].sequence}, 3) == 1, "manual candidate label applied");
    check(store.snapshotLane(0).events[0].unitId == 3, "candidate label present in atomic snapshot");
    check(store.setCandidateUnit(4, 0, {snapshot.events[0].sequence}, 2) == 0, "stale epoch cannot relabel event");
    store.addCoverageIfEpoch(0, 100, 10, 109, 5);
    ccv2::SpikeAnalysisQuality quality; QVector<qint64> exposure;
    store.snapshotAnalysis(&exposure, &quality);
    check(exposure[0] == 100 && quality.firstSourceFrame == 10 && quality.lastSourceFrame == 109, "exposure snapshot source range");
    store.markAnalysisIncomplete(5);
    check(store.snapshotLane(0).quality.incomplete(), "early stop visibly incomplete");
    const int cap = ccv2::SpikeSnippetStore::boundedCapacity(512, 1000, 2000);
    check(qint64(cap) * 512 * (1000 * sizeof(float) + sizeof(ccv2::SpikeEvent)) <= ccv2::SpikeSnippetStore::kMemoryBudgetBytes, "memory cap includes event metadata");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    storeCases();
    boundaryAndQueueCases();
    const auto a = detect(64);
    const auto b = detect(257);
    check(a.events.size() == b.events.size() && a.waves == b.waves, "waveforms invariant to chunk boundaries");
    for (int i = 0; i < std::min(a.events.size(), b.events.size()); ++i)
        check(a.events[i].sourceFrame == b.events[i].sourceFrame, "timestamps invariant to chunk boundaries");
    detect(3600, false, true, (1 << 20) - 1600, true, true); // gap inside one chunk
    detect(127, false, true, (1 << 20) - 1600, true, false); // raw counter wrap
    detect(61, true, true, 1050001, true);
    detect(73, true, false, 1050001, true);
    detect(131, false, true, 0, false, true, true);
    detect(3600, false, true, 0, false, true, false, true);
    if (failures) return 1;
    std::cout << "spike_event_smoke OK\n";
    return 0;
}
