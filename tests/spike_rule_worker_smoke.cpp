#include "service/spike_detect_worker.h"
#include "core/constants.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMutexLocker>
#include <QtEndian>
#include <atomic>
#include <cmath>
#include <iostream>

namespace {
using namespace ccv2;
int failures = 0, assertions = 0;
void check(bool ok, const char *what) {
    ++assertions;
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
SpikeDetectConfig config() {
    SpikeDetectConfig cfg;
    cfg.proc.sampleRate = 20000;
    cfg.proc.notchHz = 0;
    cfg.proc.highpassHz = 250;
    cfg.proc.spikeLowpassHz = 9000;
    cfg.proc.absoluteThreshold = true;
    cfg.proc.absThresholdV = -100e-6;
    cfg.proc.refractoryMs = 5;
    cfg.inputGain = 60;
    cfg.preSamples = 8; cfg.postSamples = 24;
    return cfg;
}
SpikeElectrodeRules rule(const SpikeRuleContext &context, int unit) {
    return {context, {{unit, {{-0.4, 1.2, -1e6, 1e6}}}}};
}
void push(const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue, int first, int count,
          const QMap<int, QVector<int>> &onsets = {{7, {100, 800, 1000, 1400, 1600}}},
          quint64 sourceEpoch = 17, qint64 invalidFrame = -1) {
    QByteArray bytes(count * kFrameBytes, '\0');
    StreamBlockInfo info; info.epoch = sourceEpoch;
    info.frameValid.fill(true, count);
    for (int j = 0; j < count; ++j) {
        const int f = first + j;
        info.frameValid[j] = f != invalidFrame;
        info.frameIndices.append(f);
        for (int ch = 0; ch < 256; ++ch) {
            int code = 2048;
            const auto it = onsets.constFind(ch);
            if (it != onsets.cend())
                for (int onset : *it) if (f >= onset && f < onset + 24) { code -= 90; break; }
            qToLittleEndian<quint32>((quint32(f) << 12) | quint32(code), bytes.data() + j * kFrameBytes + ch * 4);
        }
    }
    check(queue->push(bytes, false, nullptr, info), "lossless fixture enqueue");
}
bool waitExposure(SpikeSnippetStore &store, qint64 expected) {
    QElapsedTimer elapsed; elapsed.start();
    do {
        QVector<qint64> observed; store.snapshotAnalysis(&observed, nullptr);
        if (observed.value(255) == expected) return true;
        QThread::msleep(1);
    } while (elapsed.elapsed() < 5000);
    return false;
}
QString key(quint64 detector, quint64 rules) {
    return QString::number(detector) + ':' + QString::number(rules);
}
struct Capture {
    QMutex mutex;
    QVector<SpikeArchiveRecord> records;
    QMap<QString, QJsonObject> revisions;
    bool metadataFirst = true, workerThread = true, storeIndependent = true;
    int maxBatchSize = 0;
    void attach(SpikeDetectWorker &worker, SpikeSnippetStore &store, const QUuid &run) {
        check(worker.setEventCallbacks(run,
            [this, &worker](quint64 detector, quint64 rules, const QJsonObject &metadata) {
                QMutexLocker lock(&mutex);
                workerThread = workerThread && QThread::currentThread() == &worker;
                revisions.insert(key(detector, rules), metadata);
            },
            [this, &worker, &store](const QVector<SpikeArchiveRecord> &batch) {
                QMutexLocker lock(&mutex);
                workerThread = workerThread && QThread::currentThread() == &worker;
                maxBatchSize = std::max(maxBatchSize, int(batch.size()));
                for (const auto &record : batch) {
                    metadataFirst = metadataFirst && revisions.contains(key(record.event.detectorRevision, record.event.ruleRevision));
                    storeIndependent = storeIndependent && record.event.sequence == 0 && record.event.eventId > 0;
                    // The callback owns the complete record before any ring
                    // insertion. It is not reconstructed from retained rows.
                    const auto retained = store.snapshotLane(record.event.lane).events;
                    for (const auto &old : retained)
                        if (old.runId == record.event.runId && old.eventId == record.event.eventId) storeIndependent = false;
                    records.append(record);
                }
            }), "pre-start immutable event callbacks installed");
    }
};

void revisionBoundaryAndDrain() {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore store; check(store.configure(256, 33, 16), "revision test store configured");
    store.resetTimeline(17);
    std::atomic<quint64> epoch{17}; std::atomic_bool stop{false};
    SpikeDetectWorker worker(queue, &store, &stop, config(), &epoch);
    const auto c = worker.appliedRuleContext(7);
    check(SpikeRuleSet::validateContext(c), "worker exposes complete physical crossing context");
    Capture capture; const QUuid run = QUuid::createUuid(); capture.attach(worker, store, run);
    std::atomic<qint64> progress{0};
    QObject::connect(&worker, &SpikeDetectWorker::framesProcessed, &worker,
        [&progress](qint64 count) { progress.fetch_add(count); }, Qt::DirectConnection);
    push(queue, 0, 400);
    worker.start();
    check(waitExposure(store, 400), "initial unclassified event processed");
    auto snapshot = store.snapshotLane(7);
    check(snapshot.events.size() == 1 && snapshot.events.first().classification == SpikeClassificationStatus::Unassigned,
          "events before first rule application remain unassigned");
    const auto firstRules = SpikeRuleSet::create(1, {rule(c, 1)});
    check(worker.setRuleSet(firstRules), "future rule revision queued");
    push(queue, 400, 405);
    check(waitExposure(store, 805), "first-rule crossing captured before post-window arrives");
    SpikeAnalysisQuality quality; store.snapshotAnalysis(nullptr, &quality);
    check(quality.pendingWindowEvents == 1 && store.totalSpikes(7) == 1, "incomplete post-window remains pending");
    auto changed = c; changed.thresholdOverrideEnabled = true; changed.thresholdOverrideV = -120e-6;
    const auto secondRules = SpikeRuleSet::create(2, {rule(changed, 2)});
    check(worker.setRuleSet(secondRules), "second immutable rule queued during pending window");
    worker.setLaneThresholdOverride(7, true, -110e-6);
    worker.setLaneThresholdOverride(7, true, -120e-6); // only this complete snapshot gets applied
    push(queue, 805, 395);
    check(waitExposure(store, 1200), "pending old crossing and later new crossing completed");
    snapshot = store.snapshotLane(7);
    check(snapshot.events.size() == 3, "three completed events retained");
    if (snapshot.events.size() == 3) {
        check(snapshot.events[0].unitId == -1 && snapshot.events[0].ruleRevision == 0, "applying rules never retroactively labels prior events");
        check(snapshot.events[1].unitId == 1 && snapshot.events[1].ruleRevision == 1 && snapshot.events[1].detectorRevision == 1,
              "old pending crossing retains exact original rules and detector revision");
        check(snapshot.events[2].unitId == 2 && snapshot.events[2].ruleRevision == 2 && snapshot.events[2].detectorRevision == 2,
              "later crossing uses newly applied rules and one actual threshold revision");
    }
    auto ambiguous = rule(changed, 2); ambiguous.candidates.append(rule(changed, 3).candidates.first());
    check(worker.setRuleSet(SpikeRuleSet::create(3, {ambiguous})), "ambiguous future rule revision queued");
    check(!worker.setRuleSet(firstRules), "older/reused rule revision cannot replace current definitions");
    push(queue, 1200, 405);
    check(waitExposure(store, 1605), "ambiguous complete event and tail crossing processed");
    worker.requestDrain();
    check(worker.wait(5000) && worker.drained(), "empty-queue EOF drain finishes promptly without cancellation");
    check(queue->size() == 0 && progress.load() == 1605, "EOF drain reports final exact source-frame progress");
    store.snapshotAnalysis(nullptr, &quality);
    check(quality.pendingWindowEvents == 0 && quality.boundaryExcludedEvents == 1,
          "EOF incomplete post-window explicitly excluded rather than fabricated");
    check(std::abs(store.threshold(7) - (-120e-6)) < 1e-12, "final detector statistics published during drain");
    check(worker.appliedRuleContext(7).thresholdOverrideV == -120e-6 && worker.detectorRevision() == 2,
          "thread-safe applied context agrees with exact threshold revision");
    quint64 atomicRevision = 0;
    check(worker.appliedRuleContext(7, &atomicRevision).thresholdOverrideV == -120e-6 && atomicRevision == 2,
          "GUI context and detector revision can be captured atomically");
    check(worker.appliedRuleSet() && worker.appliedRuleSet()->revision() == 3,
          "thread-safe applied rule snapshot reflects final boundary");
    check(!worker.setEventCallbacks(run, {}, {}), "callbacks cannot change after first start");
    check(capture.workerThread && capture.metadataFirst && capture.storeIndependent,
          "direct worker callbacks precede ring insertion and always have registered metadata");
    check(capture.records.size() == 4, "all completed events captured independent of ring UI");
    for (int i = 0; i < capture.records.size(); ++i) {
        const auto &e = capture.records[i].event;
        check(e.runId == run && worker.runId() == run && e.eventId == quint64(i + 1), "stable run UUID and monotonically increasing immutable event IDs");
        check(capture.records[i].waveform.size() == 33 && capture.records[i].waveform[8] <= -100e-6,
              "callback owns correct full crossing-aligned waveform");
    }
    if (capture.records.size() == 4)
        check(capture.records[3].event.classification == SpikeClassificationStatus::Ambiguous &&
              capture.records[3].event.unitId == -1 && capture.records[3].event.ruleRevision == 3,
              "future ambiguous waveform remains explicitly ambiguous, not first-wins");
    const auto metadata = capture.revisions.value("2:2");
    const auto overrides = metadata["analysis"].toObject()["threshold_override_contexts"].toArray();
    bool exactOverride = false;
    for (const auto &value : overrides) {
        const auto o = value.toObject();
        if (o["electrode"].toInt() == 7)
            exactOverride = o["enabled"].toBool() && o["threshold_volts"].toDouble() == -120e-6;
    }
    check(exactOverride && metadata["detector_revision"].toString() == "2" &&
          metadata["rule_revision"].toString() == "2", "archived metadata records actual applied override, never intermediate requested state");
    check(QJsonDocument(metadata["rule_set"].toObject()).toJson(QJsonDocument::Compact) == secondRules->toJson(),
          "revision metadata contains the complete immutable matching rule definitions");
    check(metadata["applied_epoch"].toString() == "17" && metadata["applied_source_frame"].toString() == "805" &&
          metadata["applied_continuity_segment"].toString() == "0" &&
          metadata["boundary_semantics"].toString() == "first_input_frame_of_processing_flush",
          "pending-window edit metadata records first actual applying input frame, not crossing/completion/request time");
}

QVector<SpikeArchiveRecord> runChunked(int chunkFrames) {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore store; store.configure(256, 33, 1); store.resetTimeline(17);
    std::atomic<quint64> epoch{17}; std::atomic_bool stop{false};
    SpikeDetectWorker worker(queue, &store, &stop, config(), &epoch);
    Capture capture; capture.attach(worker, store, QUuid::createUuid());
    check(worker.setRuleSet(SpikeRuleSet::create(5, {
        rule(worker.appliedRuleContext(7), 4), rule(worker.appliedRuleContext(8), 5)})), "chunk fixture rules configured");
    const QMap<int, QVector<int>> onsets{{7, {800, 2400}}, {8, {900, 2500}}};
    for (int first = 0; first < 3600; first += chunkFrames)
        push(queue, first, std::min(chunkFrames, 3600 - first), onsets);
    worker.requestDrain(); // all producer data already queued, including final post-window
    worker.start();
    check(worker.wait(10000) && worker.drained() && queue->size() == 0, "pre-requested drain consumes all queued EOF data");
    check(capture.metadataFirst && capture.workerThread && capture.storeIndependent, "chunk run direct callback provenance valid");
    check(capture.records.size() == 4 && store.snapshotLane(7).events.size() == 1 && store.totalSpikes(7) == 2,
          "complete callback stream retains events beyond bounded display ring");
    return capture.records;
}
void chunkInvariantCases() {
    const auto large = runChunked(3600);
    for (int chunks : {17, 113}) {
        const auto smaller = runChunked(chunks);
        check(smaller.size() == large.size(), "chunk partition preserves event count");
        for (int i = 0; i < qMin(large.size(), smaller.size()); ++i) {
            const auto &a = large[i], &b = smaller[i];
            check(a.event.eventId == b.event.eventId && a.event.sourceFrame == b.event.sourceFrame &&
                  a.event.electrode == b.event.electrode && a.event.unitId == b.event.unitId &&
                  a.event.classification == b.event.classification && a.event.detectorRevision == b.event.detectorRevision &&
                  a.event.ruleRevision == b.event.ruleRevision && a.waveform == b.waveform,
                  "chunk-invariant future classification, full waveform, source identity, and deterministic IDs");
            check(a.event.runId != b.event.runId, "independent acquisitions receive different run UUIDs");
        }
    }
}

QVector<SpikeArchiveRecord> runDense(int chunkFrames) {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore store; store.configure(256, 33, 1); store.resetTimeline(17);
    std::atomic<quint64> epoch{17}; std::atomic_bool stop{false};
    SpikeDetectWorker worker(queue, &store, &stop, config(), &epoch);
    Capture capture; capture.attach(worker, store, QUuid::createUuid());
    QMap<int, QVector<int>> onsets;
    for (int lane = 0; lane < 256; ++lane) onsets.insert(lane, {100, 220, 340});
    for (int first = 0; first < 600; first += chunkFrames)
        push(queue, first, std::min(chunkFrames, 600 - first), onsets);
    worker.requestDrain(); worker.start();
    check(worker.wait(10000) && worker.drained(), "dense multi-lane EOF processing drains");
    check(capture.records.size() == 768 && capture.maxBatchSize <= 256,
          "768 multi-lane events preserved across bounded archive transport batches");
    check(capture.metadataFirst && capture.storeIndependent, "dense events remain independent of one-row rings");
    return capture.records;
}
void denseChunkInvariantCase() {
    const auto large = runDense(600), small = runDense(17);
    bool identical = large.size() == small.size();
    bool ordered = true;
    for (int i = 0; i < qMin(large.size(), small.size()); ++i) {
        const auto &a = large[i], &b = small[i];
        identical = identical && a.event.eventId == b.event.eventId && a.event.sourceFrame == b.event.sourceFrame &&
                    a.event.lane == b.event.lane && a.waveform == b.waveform;
        if (i) ordered = ordered && (a.event.sourceFrame > large[i - 1].event.sourceFrame ||
            (a.event.sourceFrame == large[i - 1].event.sourceFrame && a.event.lane > large[i - 1].event.lane));
    }
    check(identical, "dense >256 event IDs and waveforms invariant to raw chunk partition");
    check(ordered, "dense callback stream is chronologically merged across all detector lanes");
}

bool waitRevision(Capture &capture, const QString &revisionKey) {
    QElapsedTimer elapsed; elapsed.start();
    do {
        { QMutexLocker lock(&capture.mutex); if (capture.revisions.contains(revisionKey)) return true; }
        QThread::msleep(1);
    } while (elapsed.elapsed() < 5000);
    return false;
}
QJsonObject capturedRevision(Capture &capture, const QString &revisionKey) {
    QMutexLocker lock(&capture.mutex);
    return capture.revisions.value(revisionKey);
}
void appliedBoundaryProvenanceCases() {
    constexpr qint64 origin = 10000;
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore store; store.configure(256, 33, 2); store.resetTimeline(17);
    std::atomic<quint64> epoch{17}; std::atomic<int> reference{0}; std::atomic_bool stop{false};
    SpikeDetectWorker worker(queue, &store, &stop, config(), &epoch, &reference, origin);
    Capture capture; capture.attach(worker, store, QUuid::createUuid());
    const auto c = worker.appliedRuleContext(7);
    worker.start();
    check(waitRevision(capture, "1:0"), "startup configuration registered without inventing observed data");
    const auto startup = capturedRevision(capture, "1:0");
    check(startup["applied_source_frame"].toString() == "10000" && startup["applied_epoch"].toString() == "17" &&
          startup["boundary_semantics"].toString() == "subscription_origin",
          "startup provenance identifies nonzero subscription origin and initialization semantics");
    QVector<qint64> exposure; store.snapshotAnalysis(&exposure, nullptr);
    check(exposure.value(255) == 0, "subscription origin metadata does not claim processed frame exposure");
    push(queue, origin, 32, {});
    check(waitExposure(store, 32), "quiet pre-edit interval processed");
    check(worker.setRuleSet(SpikeRuleSet::create(1, {rule(c, 1)})), "quiet paused rule update requested");
    QThread::msleep(20);
    check(capturedRevision(capture, "1:1").isEmpty(), "paused/no-input rule request has no fabricated applied boundary");
    push(queue, origin + 32, 40, {});
    check(waitExposure(store, 72), "quiet post-edit interval processed");
    const auto applied = capturedRevision(capture, "1:1");
    check(applied["applied_source_frame"].toString() == "10032" && applied["applied_epoch"].toString() == "17" &&
          applied["boundary_semantics"].toString() == "first_input_frame_of_processing_flush",
          "quiet rule revision carries first actual processing frame even with zero events");
    reference.store(1);
    push(queue, origin + 72, 10, {});
    check(waitExposure(store, 82), "reference-switch interval processed");
    const auto changedReference = capturedRevision(capture, "2:1");
    check(changedReference["applied_source_frame"].toString() == "10072" &&
          changedReference["applied_continuity_segment"].toString() == "1" &&
          changedReference["analysis"].toObject()["reference_mode"].toInt() == 1,
          "reference boundary metadata describes the new continuity segment and first applied frame");
    store.resetTimeline(18); epoch.store(18);
    push(queue, 400, 1, {}, 18, 400);
    QElapsedTimer invalidWait; invalidWait.start();
    SpikeAnalysisQuality invalidQuality;
    do {
        store.snapshotAnalysis(nullptr, &invalidQuality);
        if (invalidQuality.invalidFrames == 1) break;
        QThread::msleep(1);
    } while (invalidWait.elapsed() < 5000);
    check(invalidQuality.invalidFrames == 1 && capturedRevision(capture, "3:1").isEmpty(),
          "all-invalid first epoch block defers configuration application until valid input exists");
    push(queue, 401, 63, {}, 18);
    check(waitExposure(store, 63), "new epoch valid seek interval processed");
    const auto reset = capturedRevision(capture, "3:1");
    check(reset["applied_epoch"].toString() == "18" && reset["applied_source_frame"].toString() == "401" &&
          reset["applied_continuity_segment"].toString() == "1" &&
          reset["boundary_semantics"].toString() == "epoch_reset_at_first_processed_input_frame" && worker.detectorRevision() == 3,
          "epoch filter reset registers distinct detector revision at the first actually processed valid frame");
    worker.requestDrain();
    check(worker.wait(5000) && worker.drained() && capture.records.isEmpty(),
          "quiet configuration/reset history is archived without fabricating spike events");
}
void tdmAppliedBoundaryCase() {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore store; store.configure(512, 33, 2); store.resetTimeline(17);
    std::atomic<quint64> epoch{17}; std::atomic_bool stop{false};
    auto cfg = config(); cfg.tdmEnabled = true; cfg.tdmPair02 = true; cfg.proc.sampleRate = 5000;
    SpikeDetectWorker worker(queue, &store, &stop, cfg, &epoch, nullptr, 13);
    Capture capture; capture.attach(worker, store, QUuid::createUuid());
    const auto c = worker.appliedRuleContext(0);
    worker.start();
    check(waitRevision(capture, "1:0"), "TDM startup subscription boundary recorded");
    check(worker.setRuleSet(SpikeRuleSet::create(1, {rule(c, 1)})), "TDM quiet future rules requested");
    // Frame13 is excluded phase1; frame14 is invalid phase2; frame15 is
    // excluded phase3. Frame16 is the first valid sample actually fed to DSP.
    push(queue, 13, 11, {}, 17, 14);
    worker.requestDrain();
    check(worker.wait(5000) && worker.drained(), "offset TDM invalid-first fixture drains");
    const auto applied = capturedRevision(capture, "1:1");
    check(applied["applied_source_frame"].toString() == "16" &&
          applied["applied_continuity_segment"].toString() == "1" &&
          applied["boundary_semantics"].toString() == "first_input_frame_of_processing_flush",
          "TDM application excludes hidden/invalid phase frames and names first actually fed input");
}

void cancellationAndPartialTailCases() {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore store; store.configure(256, 33, 2); store.resetTimeline(17);
    std::atomic<quint64> epoch{17}; std::atomic_bool stop{true};
    SpikeDetectWorker worker(queue, &store, &stop, config(), &epoch);
    push(queue, 0, 128);
    worker.requestDrain(); worker.start();
    check(worker.wait(5000) && !worker.drained() && queue->size() == 1,
          "immediate cancellation wins over drain and does not claim consumed queue");
    auto partialQueue = std::make_shared<ThreadSafeQueue<QByteArray>>();
    SpikeSnippetStore partialStore; partialStore.configure(256, 33, 2);
    std::atomic_bool running{false};
    SpikeDetectWorker partial(partialQueue, &partialStore, &running, config());
    partialQueue->push(QByteArray(3, '\0'));
    partial.requestDrain(); partial.start();
    check(partial.wait(5000) && partial.drained(), "partial-byte EOF terminates without spinning");
    SpikeAnalysisQuality quality; partialStore.snapshotAnalysis(nullptr, &quality);
    check(quality.invalidFrames == 1 && quality.stoppedEarly && quality.incomplete(),
          "truncated final raw frame is explicit incomplete source coverage");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    revisionBoundaryAndDrain(); chunkInvariantCases(); denseChunkInvariantCase(); appliedBoundaryProvenanceCases(); tdmAppliedBoundaryCase(); cancellationAndPartialTailCases();
    if (failures) return 1;
    std::cout << "spike_rule_worker_smoke OK (" << assertions << " assertions)\n";
    return 0;
}
