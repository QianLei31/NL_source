#include "signal/spike_sorting_model.h"
#include "signal/spike_snippet_store.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
bool near(double a, double b, double tolerance = 1e-3) { return std::abs(a - b) < tolerance; }
ccv2::SpikeEvent event(qint64 frame, quint64 epoch = 7, quint64 segment = 0) {
    ccv2::SpikeEvent e;
    e.epoch = epoch; e.continuitySegment = segment; e.sourceFrame = frame;
    e.sourceSampleRate = 20000; e.preSamples = 1; e.inputGain = 60;
    e.lane = 0; e.adcChannel = 0; e.electrode = 0;
    return e;
}
const float negative[] = {-20e-6f, -100e-6f, 20e-6f, 0};
const float positive[] = {10e-6f, 40e-6f, -10e-6f, 0};

void featureCases() {
    auto e = event(2000);
    const auto f = ccv2::SpikeSortingModel::extractFeatures(e, negative, 4);
    check(f.finiteWaveform && near(f.peakToPeakUv, 120), "P2P in input-referred microvolts");
    check(near(f.signedPeakUv, -100), "signed maximum-absolute peak preserves negative sign");
    check(near(f.energyUv2Ms, 540), "energy is sum voltage-squared times sample duration");
    e.inputGain = 180;
    check(near(ccv2::SpikeSortingModel::extractFeatures(e, negative, 4).peakToPeakUv, 120), "input gain is not applied twice");
    e.sampleStride = 4;
    check(near(ccv2::SpikeSortingModel::extractFeatures(e, negative, 4).energyUv2Ms, 2160), "TDM source stride sets energy duration");
    const auto p = ccv2::SpikeSortingModel::extractFeatures(e, positive, 4);
    check(near(p.signedPeakUv, 40) && near(p.peakToPeakUv, 50), "second deterministic unit has separate signed/P2P features");
    e.sourceSampleRate = 0;
    const auto unknown = ccv2::SpikeSortingModel::extractFeatures(e, positive, 4);
    check(unknown.finiteWaveform && std::isnan(unknown.energyUv2Ms), "unknown timebase cannot fabricate energy units");
    float bad[] = {0, std::numeric_limits<float>::quiet_NaN(), 0, 0};
    check(!ccv2::SpikeSortingModel::extractFeatures(e, bad, 4).finiteWaveform, "nonfinite waveform rejected");
}

void labelAndRetentionCases() {
    ccv2::SpikeSnippetStore store;
    check(store.configure(1, 4, 10), "configure candidate store"); store.resetTimeline(7);
    for (int i = 0; i < 12; ++i)
        check(store.addEventIfEpoch(0, i % 2 ? positive : negative, event(1000 + 20 * i)), "synthetic event accepted");
    auto snapshot = store.snapshotLane(0);
    ccv2::SpikeSortingModel model;
    check(model.setSnapshot(0, snapshot.events, snapshot.waveforms, snapshot.snippetLength), "atomic retained snapshot accepted");
    check(model.events().size() == 10 && snapshot.totalDetected == 12, "retention is distinct from all detected events");
    for (const auto &e : model.events()) check(e.unitId == -1, "unassigned by default");
    auto large = model.selectRectangle(ccv2::SpikeFeatureAxis::PeakToPeak, ccv2::SpikeFeatureAxis::SignedPeak, 110, 130, -110, -90);
    auto small = model.selectRectangle(ccv2::SpikeFeatureAxis::PeakToPeak, ccv2::SpikeFeatureAxis::Energy, 40, 60, 80, 100);
    check(large.size() == 5 && small.size() == 5, "deterministic two-unit feature rectangles select correct classes");
    QVector<quint64> seqLarge, seqSmall;
    for (int row : large) seqLarge.append(model.events()[row].sequence);
    for (int row : small) seqSmall.append(model.events()[row].sequence);
    check(store.setCandidateUnit(7, 0, seqLarge, 1) == 5, "manual first candidate labels persisted atomically");
    check(store.setCandidateUnit(7, 0, seqSmall, 2) == 5, "manual second candidate labels persisted atomically");
    snapshot = store.snapshotLane(0); model.setSnapshot(0, snapshot.events, snapshot.waveforms, 4);
    check(model.rowsForUnit(1).size() == 5 && model.rowsForUnit(2).size() == 5 && model.rowsForUnit(-1).isEmpty(), "store labels survive fresh model snapshot");
    auto qc = model.unitQc(1);
    check(qc.templateEvents == 5 && near(qc.meanUv[1], -100) && near(qc.stddevUv[1], 0), "candidate waveform mean and population SD");
    check(qc.intervalsMs.size() == 4 && near(qc.intervalsMs[0], 2) && qc.shortIntervals == 0, "unit ISI skips interleaved other-unit events");
    check(store.setCandidateUnit(7, 0, {seqLarge.first()}, -1) == 1, "explicit unassign supported");
    for (int i = 0; i < 10; ++i) store.addEventIfEpoch(0, positive, event(5000 + 20 * i));
    check(store.setCandidateUnit(7, 0, seqLarge, 9) == 0, "evicted identities cannot label replacement events");
    auto replacement = store.snapshotLane(0);
    for (const auto &e : replacement.events) check(e.unitId == -1, "new events do not inherit evicted labels");
    const auto beforeReconfigure = replacement.events.last();
    check(store.configure(2, 4, 10), "same-epoch detector reconfigure");
    store.addEventIfEpoch(0, negative, event(8000));
    replacement = store.snapshotLane(0);
    check(replacement.events.first().sequence > beforeReconfigure.sequence, "reconfigure never reuses event identity within epoch");
    check(store.setCandidateUnit(7, 0, {beforeReconfigure.sequence}, 4) == 0, "frozen pre-reconfigure selection cannot relabel new data");
    store.resetTimeline(8); store.addEventIfEpoch(0, negative, event(9000, 8));
    check(store.setCandidateUnit(7, 0, {store.snapshotLane(0).events.first().sequence}, 4) == 0, "epoch-isolated labeling rejects old epoch");
    check(store.snapshotLane(0).events.first().unitId == -1, "reset clears candidate annotations");
}

void timingAndBoundsCases() {
    QVector<ccv2::SpikeEvent> events;
    QVector<float> waves;
    for (int i = 0; i < 4; ++i) {
        auto e = event(100 + 10 * i, 7, i < 2 ? 0 : 1); e.sequence = i + 1; e.unitId = 1;
        events.append(e); for (float v : negative) waves.append(v);
    }
    ccv2::SpikeSortingModel model;
    check(model.setSnapshot(0, events, waves, 4), "continuity snapshot accepted");
    auto qc = model.unitQc(1);
    check(qc.intervalsMs.size() == 2 && qc.shortIntervals == 2, "ISI never bridges known continuity segments");
    events[3].sourceSampleRate = 10000;
    model.setSnapshot(0, events, waves, 4); qc = model.unitQc(1);
    check(qc.intervalsMs.size() == 1 && qc.incompatibleTemplates == 1, "sample-rate changes separate ISIs and templates");
    events[3].epoch = 8;
    check(!model.setSnapshot(0, events, waves, 4) && model.events().isEmpty(), "mixed epochs rejected and old model cleared");
    events[3].epoch = 7; events[3].lane = 1;
    check(!model.setSnapshot(0, events, waves, 4), "mixed lanes rejected");
    events[3].lane = 0;
    check(!model.setSnapshot(0, events, waves, 3), "metadata/waveform shape mismatch rejected");
    events.clear(); waves.clear();
    for (int i = 0; i < 3000; ++i) {
        auto e = event(i); e.sequence = i + 1; events.append(e);
        for (float v : positive) waves.append(v);
    }
    model.setSnapshot(0, events, waves, 4);
    check(model.events().size() == ccv2::SpikeSortingModel::kMaxEvents && model.omittedEvents() == 952, "explicit event bound retains newest rows");
    check(model.events().first().sequence == 953 && near(model.features().first().signedPeakUv, 40), "bounded metadata/waveform pairing preserved");
    events.resize(600);
    waves.fill(1e-6f, 600 * ccv2::SpikeSortingModel::kMaxSnippetSamples);
    check(model.setSnapshot(0, events, waves, ccv2::SpikeSortingModel::kMaxSnippetSamples), "long waveform snapshot accepted within bounded memory");
    check(model.events().size() == 512 && model.omittedEvents() == 88,
          "waveform byte budget independently bounds model below event-count limit");
    check(model.setSnapshot(0, {}, {}, 0) && model.events().isEmpty(), "empty reset snapshot clears model");
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    featureCases(); labelAndRetentionCases(); timingAndBoundsCases();
    if (failures) return 1;
    std::cout << "spike_sorting_model_smoke OK\n";
    return 0;
}
