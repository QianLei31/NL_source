#include "signal/spike_rule_classifier.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>

namespace {
using namespace ccv2;
int failures = 0;
int assertions = 0;
void check(bool ok, const char *what) {
    ++assertions;
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
SpikeRuleContext context() {
    SpikeRuleContext c;
    c.adcChannel = 0; c.electrode = 0;
    c.sourceSampleRate = 1000; c.inputGain = 60;
    c.preSamples = 1; c.postSamples = 2;
    c.absoluteThreshold = true; c.absThresholdV = -50e-6;
    return c;
}
SpikeEvent event(const SpikeRuleContext &c, qint64 sourceFrame = 1000) {
    SpikeEvent e;
    e.sourceFrame = sourceFrame; e.sourceSampleRate = c.sourceSampleRate;
    e.inputGain = c.inputGain; e.adcChannel = c.adcChannel; e.electrode = c.electrode;
    e.tdmPhase = c.tdmPhase; e.sampleStride = c.sampleStride; e.preSamples = c.preSamples;
    e.lane = 123; // deliberately unrelated to physical context
    return e;
}
SpikeElectrodeRules rules(const SpikeRuleContext &c = context()) {
    return {c, {
        {1, {{-0.05, 0.05, -120, -80}, {0.95, 1.05, 10, 30}}},
        {2, {{-0.05, 0.05, 30, 60}, {0.95, 1.05, -25, -2}}}
    }};
}
const float negative[] = {-20e-6f, -100e-6f, 20e-6f, 0};
const float positive[] = {10e-6f, 40e-6f, -10e-6f, 0};
const float neither[] = {0, 0, 0, 0};

void geometryCases() {
    auto e = event(context());
    const float rising[] = {-100e-6f, 100e-6f, 0};
    const auto hit = [&](const SpikeWaveformBox &b, const float *samples = nullptr) {
        return SpikeRuleSet::waveformIntersectsBox(b, e, samples ? samples : rising, 3);
    };
    check(hit({-0.55, -0.45, -5, 5}), "between-sample intersection is a hit without any sampled point inside");
    check(!hit({-0.95, -0.85, -5, 5}), "segment bounding-box overlap alone is not an intersection");
    check(hit({-0.75, -0.25, -0.001, 0.001}), "very thin voltage rectangle catches crossing");
    const float exact[] = {-0.125f, 0.125f, 0};
    check(hit({-0.5, -0.25, -1000, 0}, exact), "exact segment corner contact is inclusive");
    check(!hit({-0.5, -0.25, -1000, -0.000001}, exact), "near-corner nonintersection does not use broad epsilon");
    check(!hit({3, 4, -1000, 1000}), "outside waveform time range does not match");
    const float falling[] = {100e-6f, -100e-6f, 0};
    check(hit({-0.55, -0.45, -5, 5}, falling), "negative-slope between-sample intersection");
    const float flat[] = {0, 0, 0};
    check(hit({-0.5, 0.5, 0, 5}, flat), "horizontal line on inclusive voltage edge");
    check(hit({1, 2, -5, 5}, flat), "last waveform point touches inclusive time edge");
    check(!hit({1.00001, 2, -5, 5}, flat), "time just beyond endpoint does not match");
    check(!hit({-1, 1, 0.00001, 5}, flat), "horizontal line just below rectangle does not match");
    check(!hit({0, 0, -5, 5}), "zero-width box rejected");
    check(!hit({-1, 1, 0, 0}), "zero-height box rejected");
    check(!hit({1, -1, -5, 5}), "inverted time bounds rejected");
    check(!hit({-1, 1, 5, -5}), "inverted voltage bounds rejected");
    check(!hit({std::numeric_limits<double>::quiet_NaN(), 1, -5, 5}), "NaN bounds rejected");
    check(!hit({-1, std::numeric_limits<double>::infinity(), -5, 5}), "infinite bounds rejected");
    check(!hit({-1e6 - 1, 1, -5, 5}), "time coordinate bound enforced");
    check(!hit({-1, 1, -5, 1e9 + 1}), "voltage coordinate bound enforced");
    const float badLater[] = {-100e-6f, 100e-6f, std::numeric_limits<float>::quiet_NaN()};
    check(!hit({-0.55, -0.45, -5, 5}, badLater), "NaN after matching segment invalidates whole waveform");
    check(!SpikeRuleSet::waveformIntersectsBox({-1, 1, -5, 5}, e, nullptr, 3), "null waveform rejected");
    check(!SpikeRuleSet::waveformIntersectsBox({-1, 1, -5, 5}, e, flat, SpikeRuleSet::kMaxSnippetSamples + 1), "oversized length rejected before reading memory");
    e.preSamples = 0;
    check(SpikeRuleSet::waveformIntersectsBox({-1, 1, -5, 5}, e, flat, 1), "single-point waveform supported");
    e.preSamples = 1; e.sampleStride = 4;
    check(hit({-2.2, -1.8, -5, 5}), "source-frame stride determines TDM waveform time");
    e.sourceSampleRate = std::numeric_limits<double>::infinity();
    check(!hit({-1, 1, -5, 5}), "nonfinite source timebase rejected");
}

void classificationCases() {
    QString error;
    const auto c = context(); const auto e = event(c);
    auto snapshot = SpikeRuleSet::create(7, {rules()}, &error);
    check(bool(snapshot) && error.isEmpty(), "valid deterministic rules created");
    if (!snapshot) return;
    auto result = snapshot->classify(e, negative, 4, c);
    check(result.status == SpikeClassificationStatus::Assigned && result.unitId == 1 &&
          result.matchingUnitIds == QVector<int>{1} && result.ruleRevision == 7, "negative unit classified with revision");
    result = snapshot->classify(e, positive, 4, c);
    check(result.status == SpikeClassificationStatus::Assigned && result.unitId == 2, "positive unit classified");
    result = snapshot->classify(e, neither, 4, c);
    check(result.status == SpikeClassificationStatus::Unassigned && result.unitId == -1 && result.matchingUnitIds.isEmpty(), "zero matching units explicitly unassigned");
    const float firstOnly[] = {-20e-6f, -100e-6f, -50e-6f, 0};
    check(snapshot->classify(e, firstOnly, 4, c).status == SpikeClassificationStatus::Unassigned, "all boxes are AND, not OR");
    auto overlapping = rules(); overlapping.candidates.prepend({9, {overlapping.candidates[0].boxes.first()}});
    auto ambiguous = SpikeRuleSet::create(8, {overlapping});
    result = ambiguous->classify(e, negative, 4, c);
    check(result.status == SpikeClassificationStatus::Ambiguous && result.unitId == -1 &&
          result.matchingUnitIds == QVector<int>({1, 9}), "multiple matches ambiguous with sorted IDs, never first-wins");
    auto anotherElectrode = c; anotherElectrode.adcChannel = anotherElectrode.electrode = 2;
    result = snapshot->classify(event(anotherElectrode), negative, 4, anotherElectrode);
    check(result.status == SpikeClassificationStatus::Unassigned && !result.reason.isEmpty(), "unconfigured physical electrode unassigned with explanation");
    auto changedLane = e; changedLane.lane = 0;
    check(snapshot->classify(changedLane, negative, 4, c).unitId == 1, "display lane number is not rule identity");
    auto prelabelled = e; prelabelled.unitId = 32; prelabelled.classification = SpikeClassificationStatus::Manual;
    check(snapshot->classify(prelabelled, negative, 4, c).unitId == 1, "classifier computes geometry, ignores prior/manual event label");
    auto input = rules(); auto frozen = SpikeRuleSet::create(9, {input});
    auto heldAtCrossing = frozen;
    input.candidates[0].unitId = 12;
    frozen = SpikeRuleSet::create(10, {input});
    check(heldAtCrossing->classify(e, negative, 4, c).unitId == 1 && heldAtCrossing->revision() == 9 &&
          frozen->classify(e, negative, 4, c).unitId == 12 && frozen->revision() == 10,
          "held immutable snapshot preserves pending crossing after rule edit");
    check(SpikeRuleSet::create(11, {})->classify(e, negative, 4, c).status == SpikeClassificationStatus::Unassigned,
          "empty immutable revision clears future rules");
    const float bad[] = {0, std::numeric_limits<float>::infinity(), 0, 0};
    result = snapshot->classify(e, bad, 4, c);
    check(result.status == SpikeClassificationStatus::Incompatible && !result.reason.isEmpty(), "nonfinite waveform disabled with reason");
}

void compatibilityCases() {
    const auto c = context();
    const auto saved = SpikeRuleSet::create(1, {rules()});
    const QByteArray fingerprint = SpikeRuleSet::compatibilityFingerprint(c);
    check(fingerprint.size() == 64, "SHA-256 fingerprint encoded in full");
    const auto mismatch = [&](const std::function<void(SpikeRuleContext &)> &change, const char *what) {
        auto modified = c; change(modified);
        check(SpikeRuleSet::compatibilityFingerprint(modified) != fingerprint, what);
        const auto result = saved->classify(event(modified), negative, 4, modified);
        check(result.status == SpikeClassificationStatus::Incompatible && result.unitId == -1 && !result.reason.isEmpty(), what);
    };
    mismatch([](auto &x) { x.sourceSampleRate = 2000; }, "sample-rate change disables rules");
    mismatch([](auto &x) { x.inputGain = 180; }, "gain-calibration change disables rules");
    mismatch([](auto &x) { x.referenceMode = 1; }, "CAR reference change disables rules");
    mismatch([](auto &x) { x.referenceMode = 2; }, "median reference change disables rules");
    mismatch([](auto &x) { x.filterOrder = 4; }, "filter order change disables rules");
    mismatch([](auto &x) { x.notchHz = 60; }, "notch change disables rules");
    mismatch([](auto &x) { x.highpassHz = 200; }, "highpass change disables rules");
    mismatch([](auto &x) { x.spikeLowpassHz = 4000; }, "lowpass change disables rules");
    mismatch([](auto &x) { x.absoluteThreshold = false; }, "threshold mode change disables rules");
    mismatch([](auto &x) { x.absThresholdV = -60e-6; }, "absolute threshold change disables rules");
    mismatch([](auto &x) { x.rmsMultiple = 5; }, "RMS multiplier change disables rules conservatively");
    mismatch([](auto &x) { x.negativePolarity = false; }, "polarity change disables rules");
    mismatch([](auto &x) { x.refractoryMs = 2; }, "refractory change disables rules");
    mismatch([](auto &x) { x.thresholdOverrideEnabled = true; }, "threshold override mode change disables rules");
    mismatch([](auto &x) { x.thresholdOverrideV = -30e-6; }, "threshold override value change disables rules");
    mismatch([](auto &x) { x.preSamples = 2; x.postSamples = 1; }, "crossing-alignment window change disables rules");
    mismatch([](auto &x) { x.alignment = "peak"; }, "unsupported peak alignment rejected");
    mismatch([](auto &x) { x.tdmPhase = 0; x.sampleStride = 4; }, "ADC/TDM mode change disables ADC rules");
    auto tdm = c; tdm.tdmPhase = 2; tdm.adcChannel = 4; tdm.electrode = 18;
    tdm.sampleStride = 4; tdm.sourceSampleRate *= 4;
    auto tdmRules = rules(tdm);
    auto both = SpikeRuleSet::create(2, {tdmRules, rules()});
    check(bool(both) && both->classify(event(tdm), negative, 4, tdm).unitId == 1,
          "TDM physical electrode with equivalent waveform timebase classifies independently");
    auto badMetadata = event(c); badMetadata.inputGain = 180;
    check(saved->classify(badMetadata, negative, 4, c).status == SpikeClassificationStatus::Incompatible,
          "event metadata cannot conceal calibration mismatch");
    auto negzero = c; negzero.thresholdOverrideV = -0.0;
    check(SpikeRuleSet::compatibilityFingerprint(negzero) == fingerprint, "signed zero has canonical fingerprint");
    auto unknown = c; unknown.sourceSampleRate = std::numeric_limits<double>::quiet_NaN();
    check(SpikeRuleSet::compatibilityFingerprint(unknown).isEmpty(), "invalid contexts have no usable fingerprint");
}

void persistenceAndHeldOutCases() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary rule persistence directory");
    const auto c = context();
    const quint64 revision = std::numeric_limits<quint64>::max();
    auto trained = SpikeRuleSet::create(revision, {rules()});
    QString error;
    const QByteArray original = trained->toJson();
    check(trained->save(dir.filePath("candidate-rules.json"), &error), "atomic rules file saved");
    trained.reset(); // simulates destruction of the old acquisition/rule manager
    const auto restarted = SpikeRuleSet::load(dir.filePath("candidate-rules.json"), &error);
    check(bool(restarted) && restarted->revision() == revision && restarted->toJson() == original,
          "restart reload preserves exact uint64 revision and deterministic JSON");
    if (!restarted) return;
    // Held-out later synthetic events: the truth class chooses a generative
    // waveform family only. Event labels start deliberately wrong; all reported
    // labels below are computed by the reloaded classifier, never copied truth.
    int correct = 0, incorrect = 0, unassigned = 0;
    for (int i = 0; i < 240; ++i) {
        const int truth = (i % 3) + 1;
        const double jitter = std::sin(i * 1.231) * 1.2;
        const double scale = 1 + 0.035 * std::cos(i * 0.731);
        const float *prototype = truth == 1 ? negative : truth == 2 ? positive : neither;
        float waveform[4];
        for (int s = 0; s < 4; ++s) waveform[s] = float(prototype[s] * scale + jitter * 1e-6 * (s == 1 ? 1 : 0.3));
        auto later = event(c, 1000000 + i * 50);
        later.eventId = quint64(i + 1); later.unitId = 32;
        const auto classified = restarted->classify(later, waveform, 4, c);
        later.unitId = classified.unitId;
        later.classification = classified.status;
        later.ruleRevision = classified.ruleRevision;
        if (truth == 3) {
            if (later.classification == SpikeClassificationStatus::Unassigned && later.unitId == -1) ++unassigned;
            else ++incorrect;
        } else if (later.classification == SpikeClassificationStatus::Assigned && later.unitId == truth) ++correct;
        else ++incorrect;
    }
    check(correct == 160 && unassigned == 80 && incorrect == 0,
          "reloaded classifier assigns 160 held-out later events and leaves 80 noise events unassigned");
    auto changedOrder = rules(); std::reverse(changedOrder.candidates.begin(), changedOrder.candidates.end());
    check(SpikeRuleSet::create(revision, {changedOrder})->toJson() == original, "candidate serialization canonical despite editor insertion order");
    check(!SpikeRuleSet::load(dir.filePath("absent.json"), &error) && !error.isEmpty(), "missing rule file gives actionable failure");
    check(!restarted->save(dir.filePath("missing/rules.json"), &error) && !error.isEmpty(), "failed save does not claim success");
}

void invalidAndMaliciousCases() {
    QString error;
    auto e = rules();
    check(!SpikeRuleSet::create(0, {e}, &error) && !error.isEmpty(), "revision zero reserved for no captured rules");
    auto bad = e; bad.candidates[0].unitId = 0;
    check(!SpikeRuleSet::create(1, {bad}), "candidate zero invalid");
    bad = e; bad.candidates[0].unitId = 33;
    check(!SpikeRuleSet::create(1, {bad}), "candidate above 32 invalid");
    bad = e; bad.candidates[1].unitId = 1;
    check(!SpikeRuleSet::create(1, {bad}), "duplicate candidate ID invalid");
    bad = e; bad.candidates[0].boxes.clear();
    check(!SpikeRuleSet::create(1, {bad}), "empty AND cannot match every waveform");
    bad = e; bad.candidates.clear();
    check(!SpikeRuleSet::create(1, {bad}), "empty electrode group invalid, omit group instead");
    bad = e; bad.candidates[0].boxes.fill(e.candidates[0].boxes.first(), SpikeRuleSet::kMaxBoxesPerUnit + 1);
    check(!SpikeRuleSet::create(1, {bad}), "per-unit box budget enforced");
    bad = e; bad.context.electrode = 3;
    check(!SpikeRuleSet::create(1, {bad}), "ADC physical mapping validated");
    bad = e; bad.context.inputGain = 0;
    check(!SpikeRuleSet::create(1, {bad}), "gain must be strictly positive");
    bad = e; bad.context.preSamples = std::numeric_limits<int>::max();
    check(!SpikeRuleSet::create(1, {bad}), "window integer overflow cannot evade bound");
    check(!SpikeRuleSet::create(1, {e, e}), "duplicate physical context rejected");
    const auto valid = SpikeRuleSet::create(1, {e})->toJson();
    const auto reject = [&](QByteArray bytes, const char *what) {
        error.clear(); check(!SpikeRuleSet::fromJson(bytes, &error) && !error.isEmpty(), what);
    };
    reject({}, "empty JSON rejected");
    reject(QByteArray(SpikeRuleSet::kMaxJsonBytes + 1, ' '), "oversized JSON rejected before parse");
    reject(QByteArray(30, '[') + "0" + QByteArray(30, ']'), "deep malicious JSON rejected before parse");
    reject("{\"schema\":\"a\",\"schema\":\"b\"}", "duplicate keys rejected, never last-wins");
    reject("{\"schema\":\"a\",\"\\u0073chema\":\"b\"}", "escaped duplicate keys rejected");
    reject("{\"schema\":\"" + QByteArray(1000, 'x') + "\"}", "oversized strings rejected");
    reject("{}garbage", "trailing malformed JSON rejected");
    const auto mutate = [&](const std::function<void(QJsonObject &)> &change, const char *what) {
        auto root = QJsonDocument::fromJson(valid).object(); change(root);
        reject(QJsonDocument(root).toJson(QJsonDocument::Compact), what);
    };
    mutate([](auto &o) { o["unknown"] = 1; }, "unknown root fields rejected");
    mutate([](auto &o) { o["schema_version"] = 2; }, "future unsupported schema version rejected");
    mutate([](auto &o) { o["schema_version"] = 1.5; }, "fractional integer rejected");
    mutate([](auto &o) { o["rule_revision"] = 9007199254740992.0; }, "numeric revision rejected to preserve integer precision");
    mutate([](auto &o) { o["rule_revision"] = "18446744073709551616"; }, "uint64 overflow revision rejected");
    mutate([](auto &o) { o["rule_revision"] = "01"; }, "noncanonical revision rejected");
    mutate([](auto &o) {
        auto a = o["electrodes"].toArray(); auto g = a[0].toObject();
        g["compatibility_sha256"] = QString(64, '0'); a[0] = g; o["electrodes"] = a;
    }, "tampered compatibility fingerprint rejected");
    mutate([](auto &o) {
        auto a = o["electrodes"].toArray(); auto g = a[0].toObject(); auto c = g["context"].toObject();
        c["input_gain"] = 61; g["context"] = c; a[0] = g; o["electrodes"] = a;
    }, "context edited without matching fingerprint rejected");
    mutate([](auto &o) {
        auto a = o["electrodes"].toArray(); auto g = a[0].toObject(); auto c = g["context"].toObject();
        c["threshold_absolute"] = 1; g["context"] = c; a[0] = g; o["electrodes"] = a;
    }, "boolean type is strict, numeric coercion refused");
    mutate([](auto &o) {
        auto a = o["electrodes"].toArray(); auto g = a[0].toObject(); auto units = g["candidates"].toArray();
        auto u = units[0].toObject(); auto boxes = u["boxes"].toArray(); auto box = boxes[0].toObject();
        box["voltage_min_uv"] = QJsonValue::Null; boxes[0] = box; u["boxes"] = boxes; units[0] = u;
        g["candidates"] = units; a[0] = g; o["electrodes"] = a;
    }, "JSON null cannot masquerade as nonfinite numeric bounds");
    mutate([](auto &o) {
        auto a = o["electrodes"].toArray(); auto g = a[0].toObject(); auto units = g["candidates"].toArray();
        auto u = units[0].toObject(); QJsonArray boxes;
        for (int i = 0; i <= SpikeRuleSet::kMaxBoxesPerUnit; ++i) boxes.append(u["boxes"].toArray()[0]);
        u["boxes"] = boxes; units[0] = u; g["candidates"] = units; a[0] = g; o["electrodes"] = a;
    }, "malicious oversized box array bounded");
    mutate([](auto &o) {
        const auto value = o["electrodes"].toArray()[0]; QJsonArray a;
        for (int i = 0; i <= SpikeRuleSet::kMaxElectrodes; ++i) a.append(value);
        o["electrodes"] = a;
    }, "malicious oversized electrode array bounded");
    QVector<SpikeElectrodeRules> many;
    for (int channel = 0; channel < 17; ++channel) {
        auto group = e; group.context.adcChannel = group.context.electrode = channel; group.candidates.clear();
        for (int unit = 1; unit <= 32; ++unit) {
            SpikeCandidateRule candidate; candidate.unitId = unit;
            candidate.boxes.fill(e.candidates[0].boxes[0], 16); group.candidates.append(candidate);
        }
        many.append(group);
    }
    check(!SpikeRuleSet::create(1, many, &error) && error.contains("Total"), "global box budget bounded independently of local limits");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    geometryCases(); classificationCases(); compatibilityCases();
    persistenceAndHeldOutCases(); invalidAndMaliciousCases();
    if (failures) return 1;
    std::cout << "spike_rule_classifier_smoke OK (" << assertions << " assertions; 240 held-out later synthetic events)\n";
    return 0;
}
