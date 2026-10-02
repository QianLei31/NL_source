#include "signal/spike_rule_classifier.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace ccv2 {
namespace {
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}
bool finiteRange(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
double zeroNormalized(double value) { return value == 0 ? 0.0 : value; }

auto contextKey(const SpikeRuleContext &c) {
    return std::make_tuple(c.tdmPhase >= 0, c.electrode, c.adcChannel, c.tdmPhase);
}

QJsonObject contextJson(const SpikeRuleContext &c) {
    QJsonObject o;
    o["adc_channel"] = c.adcChannel;
    o["electrode"] = c.electrode;
    o["tdm_phase"] = c.tdmPhase;
    o["sample_stride"] = c.sampleStride;
    o["source_sample_rate_hz"] = c.sourceSampleRate;
    o["input_gain"] = c.inputGain;
    o["reference_mode"] = c.referenceMode;
    o["filter_order"] = c.filterOrder;
    o["notch_hz"] = c.notchHz;
    o["highpass_hz"] = c.highpassHz;
    o["spike_lowpass_hz"] = c.spikeLowpassHz;
    o["threshold_absolute"] = c.absoluteThreshold;
    o["threshold_volts"] = c.absThresholdV;
    o["rms_multiple"] = c.rmsMultiple;
    o["negative_polarity"] = c.negativePolarity;
    o["refractory_ms"] = c.refractoryMs;
    o["threshold_override_enabled"] = c.thresholdOverrideEnabled;
    o["threshold_override_volts"] = c.thresholdOverrideV;
    o["alignment"] = c.alignment;
    o["pre_samples"] = c.preSamples;
    o["post_samples"] = c.postSamples;
    return o;
}

bool exactKeys(const QJsonObject &o, std::initializer_list<const char *> names,
               QString *error) {
    if (o.size() != qsizetype(names.size()))
        return fail(error, QStringLiteral("Missing or unknown JSON fields"));
    for (const char *name : names)
        if (!o.contains(QLatin1String(name)))
            return fail(error, QStringLiteral("Missing JSON field: ") + QLatin1String(name));
    return true;
}

bool number(const QJsonObject &o, const char *key, double &out, QString *error) {
    const auto value = o.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return fail(error, QStringLiteral("Expected finite number: ") + QLatin1String(key));
    out = zeroNormalized(value.toDouble());
    return true;
}
bool integer(const QJsonObject &o, const char *key, int &out, QString *error) {
    double value;
    if (!number(o, key, value, error)) return false;
    if (value != std::floor(value) || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max())
        return fail(error, QStringLiteral("Expected bounded integer: ") + QLatin1String(key));
    out = int(value);
    return true;
}
bool boolean(const QJsonObject &o, const char *key, bool &out, QString *error) {
    const auto value = o.value(QLatin1String(key));
    if (!value.isBool())
        return fail(error, QStringLiteral("Expected boolean: ") + QLatin1String(key));
    out = value.toBool();
    return true;
}

bool readContext(const QJsonObject &o, SpikeRuleContext &c, QString *error) {
    if (!exactKeys(o, {"adc_channel", "electrode", "tdm_phase", "sample_stride",
        "source_sample_rate_hz", "input_gain", "reference_mode", "filter_order",
        "notch_hz", "highpass_hz", "spike_lowpass_hz", "threshold_absolute",
        "threshold_volts", "rms_multiple", "negative_polarity", "refractory_ms",
        "threshold_override_enabled", "threshold_override_volts", "alignment",
        "pre_samples", "post_samples"}, error)) return false;
    if (!integer(o, "adc_channel", c.adcChannel, error) ||
        !integer(o, "electrode", c.electrode, error) ||
        !integer(o, "tdm_phase", c.tdmPhase, error) ||
        !integer(o, "sample_stride", c.sampleStride, error) ||
        !number(o, "source_sample_rate_hz", c.sourceSampleRate, error) ||
        !number(o, "input_gain", c.inputGain, error) ||
        !integer(o, "reference_mode", c.referenceMode, error) ||
        !integer(o, "filter_order", c.filterOrder, error) ||
        !integer(o, "notch_hz", c.notchHz, error) ||
        !number(o, "highpass_hz", c.highpassHz, error) ||
        !number(o, "spike_lowpass_hz", c.spikeLowpassHz, error) ||
        !boolean(o, "threshold_absolute", c.absoluteThreshold, error) ||
        !number(o, "threshold_volts", c.absThresholdV, error) ||
        !number(o, "rms_multiple", c.rmsMultiple, error) ||
        !boolean(o, "negative_polarity", c.negativePolarity, error) ||
        !number(o, "refractory_ms", c.refractoryMs, error) ||
        !boolean(o, "threshold_override_enabled", c.thresholdOverrideEnabled, error) ||
        !number(o, "threshold_override_volts", c.thresholdOverrideV, error) ||
        !integer(o, "pre_samples", c.preSamples, error) ||
        !integer(o, "post_samples", c.postSamples, error)) return false;
    if (!o["alignment"].isString())
        return fail(error, QStringLiteral("Expected alignment string"));
    c.alignment = o["alignment"].toString();
    return SpikeRuleSet::validateContext(c, error);
}

// Bound parser work before constructing a document, and reject duplicate keys
// (QJsonDocument normally silently keeps their last value).
bool jsonPreflight(const QByteArray &json, QString *error) {
    if (json.isEmpty() || json.size() > SpikeRuleSet::kMaxJsonBytes)
        return fail(error, QStringLiteral("Rule JSON is empty or exceeds 4 MiB"));
    struct Frame { char opener; QSet<QString> keys; };
    QVector<Frame> stack;
    auto space = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
    for (qsizetype i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            const qsizetype start = i;
            bool ended = false;
            while (++i < json.size()) {
                if (json[i] == '\\') { ++i; continue; }
                if (json[i] == '"') { ended = true; break; }
            }
            if (!ended || i - start > 768)
                return fail(error, QStringLiteral("Invalid or oversized JSON string"));
            QJsonParseError parseError;
            const auto stringDoc = QJsonDocument::fromJson(
                QByteArray("[") + json.mid(start, i - start + 1) + "]", &parseError);
            if (parseError.error != QJsonParseError::NoError ||
                stringDoc.array().first().toString().size() > 128)
                return fail(error, QStringLiteral("Invalid or oversized JSON string"));
            qsizetype next = i + 1;
            while (next < json.size() && space(json[next])) ++next;
            if (next < json.size() && json[next] == ':' && !stack.isEmpty() &&
                stack.last().opener == '{') {
                const QString key = stringDoc.array().first().toString();
                if (stack.last().keys.contains(key))
                    return fail(error, QStringLiteral("Duplicate JSON field: ") + key);
                if (stack.last().keys.size() >= 32)
                    return fail(error, QStringLiteral("Too many JSON object fields"));
                stack.last().keys.insert(key);
            }
        } else if (c == '{' || c == '[') {
            if (stack.size() >= 12)
                return fail(error, QStringLiteral("Rule JSON nesting exceeds limit"));
            stack.append(Frame{c, {}});
        } else if (c == '}' || c == ']') {
            if (stack.isEmpty() || stack.last().opener != (c == '}' ? '{' : '['))
                return fail(error, QStringLiteral("Mismatched JSON container"));
            stack.removeLast();
        }
    }
    return stack.isEmpty() || fail(error, QStringLiteral("Unterminated JSON container"));
}

bool validWaveform(const SpikeEvent &event, const float *samples, int length) {
    if (!samples || length < 1 || length > SpikeRuleSet::kMaxSnippetSamples ||
        !finiteRange(event.sourceSampleRate, 1.0, 1e9) ||
        (event.sampleStride != 1 && event.sampleStride != 4) ||
        event.preSamples < 0 || event.preSamples >= length) return false;
    for (int i = 0; i < length; ++i)
        if (!std::isfinite(samples[i])) return false;
    return true;
}

bool intersects(const SpikeWaveformBox &b, const SpikeEvent &event,
                const float *samples, int length) {
    const double step = 1000.0 * event.sampleStride / event.sourceSampleRate;
    if (length == 1)
        return b.timeMinMs <= 0 && b.timeMaxMs >= 0 &&
               samples[0] * 1e6 >= b.voltageMinUv && samples[0] * 1e6 <= b.voltageMaxUv;
    for (int i = 1; i < length; ++i) {
        const double t0 = (i - 1 - event.preSamples) * step;
        const double t1 = (i - event.preSamples) * step;
        const double left = std::max(t0, b.timeMinMs);
        const double right = std::min(t1, b.timeMaxMs);
        if (left > right) continue;
        const double y0 = samples[i - 1] * 1e6;
        const double y1 = samples[i] * 1e6;
        const double yl = y0 + (y1 - y0) * ((left - t0) / (t1 - t0));
        const double yr = y0 + (y1 - y0) * ((right - t0) / (t1 - t0));
        if (std::min(yl, yr) <= b.voltageMaxUv && std::max(yl, yr) >= b.voltageMinUv)
            return true;
    }
    return false;
}

QString incompatibility(const SpikeRuleContext &saved, const SpikeRuleContext &live) {
    if (contextKey(saved) != contextKey(live) || saved.sampleStride != live.sampleStride)
        return QStringLiteral("Physical electrode or ADC/TDM mode differs from saved rule");
    if (saved.sourceSampleRate != live.sourceSampleRate)
        return QStringLiteral("Source sample rate differs from saved rule");
    if (saved.inputGain != live.inputGain)
        return QStringLiteral("Input-gain calibration differs from saved rule");
    if (saved.referenceMode != live.referenceMode)
        return QStringLiteral("Software reference differs from saved rule");
    if (saved.filterOrder != live.filterOrder || saved.notchHz != live.notchHz ||
        saved.highpassHz != live.highpassHz || saved.spikeLowpassHz != live.spikeLowpassHz)
        return QStringLiteral("Spike filter differs from saved rule");
    if (saved.alignment != live.alignment || saved.preSamples != live.preSamples ||
        saved.postSamples != live.postSamples)
        return QStringLiteral("Waveform alignment or window differs from saved rule");
    return QStringLiteral("Threshold or refractory configuration differs from saved rule");
}
} // namespace

bool SpikeRuleSet::validateContext(const SpikeRuleContext &c, QString *error) {
    if (error) error->clear();
    if (c.adcChannel < 0 || c.adcChannel > 255 || c.electrode < 0 || c.electrode > 1023 ||
        c.tdmPhase < -1 || c.tdmPhase > 3 ||
        (c.tdmPhase == -1 && (c.sampleStride != 1 || c.electrode != c.adcChannel)) ||
        (c.tdmPhase >= 0 && (c.sampleStride != 4 || c.electrode != c.adcChannel * 4 + c.tdmPhase)))
        return fail(error, QStringLiteral("Invalid physical electrode/ADC/TDM context"));
    if (!finiteRange(c.sourceSampleRate, 1.0, 1e9) ||
        !finiteRange(c.inputGain, std::numeric_limits<double>::min(), 1e9))
        return fail(error, QStringLiteral("Invalid sample rate or input-gain calibration"));
    if (c.referenceMode < 0 || c.referenceMode > 2 ||
        (c.filterOrder != 2 && c.filterOrder != 4 && c.filterOrder != 6 && c.filterOrder != 8) ||
        (c.notchHz != 0 && c.notchHz != 50 && c.notchHz != 60) ||
        !finiteRange(c.highpassHz, 0.0, 1e9) || !finiteRange(c.spikeLowpassHz, 0.0, 1e9))
        return fail(error, QStringLiteral("Invalid reference or spike-filter configuration"));
    if (!finiteRange(c.absThresholdV, -1e3, 1e3) ||
        !finiteRange(c.rmsMultiple, 0.0, 1e3) ||
        !finiteRange(c.refractoryMs, 0.0, 1e6) ||
        !finiteRange(c.thresholdOverrideV, -1e3, 1e3))
        return fail(error, QStringLiteral("Invalid threshold or refractory configuration"));
    if (c.alignment != QLatin1String("causal_threshold_crossing_not_peak") ||
        c.preSamples < 0 || c.postSamples < 0 ||
        qint64(c.preSamples) + c.postSamples + 1 > kMaxSnippetSamples)
        return fail(error, QStringLiteral("Unsupported alignment or waveform window"));
    return true;
}

bool SpikeRuleSet::validateBox(const SpikeWaveformBox &b, QString *error) {
    if (error) error->clear();
    if (!finiteRange(b.timeMinMs, -1e6, 1e6) || !finiteRange(b.timeMaxMs, -1e6, 1e6) ||
        !finiteRange(b.voltageMinUv, -1e9, 1e9) || !finiteRange(b.voltageMaxUv, -1e9, 1e9) ||
        b.timeMinMs >= b.timeMaxMs || b.voltageMinUv >= b.voltageMaxUv)
        return fail(error, QStringLiteral("Waveform box must have finite, bounded, increasing coordinates"));
    return true;
}

QByteArray SpikeRuleSet::compatibilityFingerprint(const SpikeRuleContext &c) {
    if (!validateContext(c)) return {};
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << QByteArray("nl-spike-rule-context-v1")
           << qint32(c.adcChannel) << qint32(c.electrode) << qint32(c.tdmPhase)
           << qint32(c.sampleStride) << zeroNormalized(c.sourceSampleRate) << zeroNormalized(c.inputGain)
           << qint32(c.referenceMode) << qint32(c.filterOrder) << qint32(c.notchHz)
           << zeroNormalized(c.highpassHz) << zeroNormalized(c.spikeLowpassHz)
           << c.absoluteThreshold << zeroNormalized(c.absThresholdV) << zeroNormalized(c.rmsMultiple)
           << c.negativePolarity << zeroNormalized(c.refractoryMs)
           << c.thresholdOverrideEnabled << zeroNormalized(c.thresholdOverrideV)
           << c.alignment << qint32(c.preSamples) << qint32(c.postSamples);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

SpikeRuleSet::SpikeRuleSet(quint64 revision, QVector<SpikeElectrodeRules> electrodes)
    : m_revision(revision), m_electrodes(std::move(electrodes)) {
    std::sort(m_electrodes.begin(), m_electrodes.end(), [](const auto &a, const auto &b) {
        return contextKey(a.context) < contextKey(b.context);
    });
    for (auto &e : m_electrodes) {
        std::sort(e.candidates.begin(), e.candidates.end(), [](const auto &a, const auto &b) {
            return a.unitId < b.unitId;
        });
        m_fingerprints.append(compatibilityFingerprint(e.context));
    }
}

SpikeRuleSet::Snapshot SpikeRuleSet::create(quint64 revision,
                                           const QVector<SpikeElectrodeRules> &electrodes,
                                           QString *error) {
    if (error) error->clear();
    if (!revision || electrodes.size() > kMaxElectrodes) {
        fail(error, QStringLiteral("Rule revision must be nonzero; electrode count exceeds limit"));
        return {};
    }
    QSet<QString> keys;
    int totalBoxes = 0;
    for (const auto &e : electrodes) {
        if (!validateContext(e.context, error)) return {};
        const QString key = QString::number(e.context.tdmPhase >= 0) + ':' + QString::number(e.context.electrode);
        if (keys.contains(key) || e.candidates.isEmpty() || e.candidates.size() > kMaxUnits) {
            fail(error, QStringLiteral("Duplicate electrode context or invalid candidate count"));
            return {};
        }
        keys.insert(key);
        QSet<int> units;
        for (const auto &unit : e.candidates) {
            if (unit.unitId < 1 || unit.unitId > kMaxUnits || units.contains(unit.unitId) ||
                unit.boxes.isEmpty() || unit.boxes.size() > kMaxBoxesPerUnit) {
                fail(error, QStringLiteral("Invalid/duplicate unit ID or invalid waveform-box count"));
                return {};
            }
            units.insert(unit.unitId);
            totalBoxes += int(unit.boxes.size());
            if (totalBoxes > kMaxTotalBoxes) {
                fail(error, QStringLiteral("Total waveform-box count exceeds limit"));
                return {};
            }
            for (const auto &box : unit.boxes)
                if (!validateBox(box, error)) return {};
        }
    }
    return Snapshot(new SpikeRuleSet(revision, electrodes));
}

QByteArray SpikeRuleSet::toJson() const {
    QJsonObject root;
    root["schema"] = "nl_spike_waveform_rules";
    root["schema_version"] = kSchemaVersion;
    root["rule_revision"] = QString::number(m_revision); // uint64, lossless in JSON
    root["classification_semantics"] = "candidate_labels_not_validated_neuron_isolation";
    QJsonArray electrodes;
    for (int i = 0; i < m_electrodes.size(); ++i) {
        const auto &e = m_electrodes[i];
        QJsonObject entry;
        entry["context"] = contextJson(e.context);
        entry["compatibility_sha256"] = QString::fromLatin1(m_fingerprints[i]);
        QJsonArray units;
        for (const auto &unit : e.candidates) {
            QJsonObject u;
            u["unit_id"] = unit.unitId;
            QJsonArray boxes;
            for (const auto &b : unit.boxes) {
                QJsonObject box;
                box["time_min_ms"] = b.timeMinMs; box["time_max_ms"] = b.timeMaxMs;
                box["voltage_min_uv"] = b.voltageMinUv; box["voltage_max_uv"] = b.voltageMaxUv;
                boxes.append(box);
            }
            u["boxes"] = boxes;
            units.append(u);
        }
        entry["candidates"] = units;
        electrodes.append(entry);
    }
    root["electrodes"] = electrodes;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

SpikeRuleSet::Snapshot SpikeRuleSet::fromJson(const QByteArray &json, QString *error) {
    if (error) error->clear();
    if (!jsonPreflight(json, error)) return {};
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, QStringLiteral("Malformed rule JSON: ") + parseError.errorString());
        return {};
    }
    const auto root = document.object();
    int version = 0;
    if (!exactKeys(root, {"schema", "schema_version", "rule_revision", "classification_semantics", "electrodes"}, error) ||
        !integer(root, "schema_version", version, error)) return {};
    if (root["schema"].toString() != QLatin1String("nl_spike_waveform_rules") || version != kSchemaVersion ||
        root["classification_semantics"].toString() != QLatin1String("candidate_labels_not_validated_neuron_isolation") ||
        !root["rule_revision"].isString() || !root["electrodes"].isArray()) {
        fail(error, QStringLiteral("Unsupported rule schema, semantics, revision, or electrode array"));
        return {};
    }
    const QString revisionString = root["rule_revision"].toString();
    bool ok;
    const quint64 revision = revisionString.toULongLong(&ok);
    if (!ok || !revision || revisionString != QString::number(revision)) {
        fail(error, QStringLiteral("Rule revision must be a canonical nonzero uint64 decimal string"));
        return {};
    }
    const auto array = root["electrodes"].toArray();
    if (array.size() > kMaxElectrodes) {
        fail(error, QStringLiteral("Electrode count exceeds limit"));
        return {};
    }
    QVector<SpikeElectrodeRules> electrodes;
    int totalBoxes = 0;
    for (const auto &value : array) {
        if (!value.isObject()) { fail(error, QStringLiteral("Electrode entry must be an object")); return {}; }
        const auto entry = value.toObject();
        if (!exactKeys(entry, {"context", "compatibility_sha256", "candidates"}, error)) return {};
        SpikeElectrodeRules e;
        if (!entry["context"].isObject() || !entry["candidates"].isArray()) {
            fail(error, QStringLiteral("Invalid electrode context or candidate array")); return {};
        }
        if (!readContext(entry["context"].toObject(), e.context, error)) return {};
        const QString fingerprint = entry["compatibility_sha256"].toString();
        if (fingerprint.size() != 64 || fingerprint.toLatin1() != compatibilityFingerprint(e.context)) {
            fail(error, QStringLiteral("Context compatibility fingerprint is missing or does not match")); return {};
        }
        const auto units = entry["candidates"].toArray();
        if (units.isEmpty() || units.size() > kMaxUnits) {
            fail(error, QStringLiteral("Candidate count exceeds bounds")); return {};
        }
        for (const auto &unitValue : units) {
            if (!unitValue.isObject()) { fail(error, QStringLiteral("Candidate must be an object")); return {}; }
            const auto u = unitValue.toObject();
            if (!exactKeys(u, {"unit_id", "boxes"}, error)) return {};
            SpikeCandidateRule unit;
            if (!integer(u, "unit_id", unit.unitId, error)) return {};
            if (!u["boxes"].isArray()) { fail(error, QStringLiteral("Boxes must be an array")); return {}; }
            const auto boxes = u["boxes"].toArray();
            if (boxes.isEmpty() || boxes.size() > kMaxBoxesPerUnit ||
                (totalBoxes += int(boxes.size())) > kMaxTotalBoxes) {
                fail(error, QStringLiteral("Waveform-box count exceeds bounds")); return {};
            }
            for (const auto &boxValue : boxes) {
                if (!boxValue.isObject()) { fail(error, QStringLiteral("Box must be an object")); return {}; }
                const auto b = boxValue.toObject();
                if (!exactKeys(b, {"time_min_ms", "time_max_ms", "voltage_min_uv", "voltage_max_uv"}, error)) return {};
                SpikeWaveformBox box;
                if (!number(b, "time_min_ms", box.timeMinMs, error) ||
                    !number(b, "time_max_ms", box.timeMaxMs, error) ||
                    !number(b, "voltage_min_uv", box.voltageMinUv, error) ||
                    !number(b, "voltage_max_uv", box.voltageMaxUv, error) ||
                    !validateBox(box, error)) return {};
                unit.boxes.append(box);
            }
            e.candidates.append(unit);
        }
        electrodes.append(e);
    }
    return create(revision, electrodes, error);
}

SpikeRuleSet::Snapshot SpikeRuleSet::load(const QString &path, QString *error) {
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { fail(error, file.errorString()); return {}; }
    if (file.size() <= 0 || file.size() > kMaxJsonBytes) {
        fail(error, QStringLiteral("Rule file is empty or exceeds 4 MiB")); return {};
    }
    const QByteArray json = file.read(kMaxJsonBytes + 1LL);
    if (file.error() != QFileDevice::NoError) { fail(error, file.errorString()); return {}; }
    if (!file.atEnd()) { fail(error, QStringLiteral("Rule file exceeds bounded read")); return {}; }
    return fromJson(json, error);
}

bool SpikeRuleSet::save(const QString &path, QString *error) const {
    if (error) error->clear();
    const QByteArray json = toJson();
    if (json.size() > kMaxJsonBytes)
        return fail(error, QStringLiteral("Serialized rule set exceeds 4 MiB"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    if (file.write(json) != json.size()) return fail(error, file.errorString());
    if (!file.commit()) return fail(error, file.errorString());
    return true;
}

bool SpikeRuleSet::waveformIntersectsBox(const SpikeWaveformBox &box, const SpikeEvent &event,
                                        const float *samples, int length) {
    return validateBox(box) && validWaveform(event, samples, length) &&
           intersects(box, event, samples, length);
}

SpikeRuleClassification SpikeRuleSet::classify(const SpikeEvent &event, const float *samples,
                                               int length, const SpikeRuleContext &context) const {
    SpikeRuleClassification result;
    result.ruleRevision = m_revision;
    auto incompatible = [&](const QString &reason) {
        result.status = SpikeClassificationStatus::Incompatible;
        result.reason = reason;
        return result;
    };
    QString validationError;
    if (!validateContext(context, &validationError)) return incompatible(validationError);
    if (!validWaveform(event, samples, length))
        return incompatible(QStringLiteral("Waveform or source timebase is invalid/nonfinite"));
    if (event.adcChannel != context.adcChannel || event.electrode != context.electrode ||
        event.tdmPhase != context.tdmPhase || event.sampleStride != context.sampleStride ||
        event.sourceSampleRate != context.sourceSampleRate || event.inputGain != context.inputGain ||
        event.preSamples != context.preSamples || length != context.preSamples + context.postSamples + 1)
        return incompatible(QStringLiteral("Event metadata differs from its crossing context"));
    int index = -1;
    bool otherMode = false;
    for (int i = 0; i < m_electrodes.size(); ++i) {
        const auto &saved = m_electrodes[i].context;
        if (contextKey(saved) == contextKey(context)) { index = i; break; }
        if (saved.electrode == context.electrode) otherMode = true;
    }
    if (index < 0) {
        if (otherMode) return incompatible(QStringLiteral("Saved electrode rule belongs to another ADC/TDM context"));
        result.reason = QStringLiteral("No candidate rules for this physical electrode");
        return result;
    }
    if (compatibilityFingerprint(context) != m_fingerprints[index])
        return incompatible(incompatibility(m_electrodes[index].context, context));
    for (const auto &unit : m_electrodes[index].candidates) {
        bool matches = true;
        for (const auto &box : unit.boxes)
            if (!intersects(box, event, samples, length)) { matches = false; break; }
        if (matches) result.matchingUnitIds.append(unit.unitId);
    }
    if (result.matchingUnitIds.size() == 1) {
        result.status = SpikeClassificationStatus::Assigned;
        result.unitId = result.matchingUnitIds.first();
    } else if (result.matchingUnitIds.size() > 1) {
        result.status = SpikeClassificationStatus::Ambiguous;
        result.reason = QStringLiteral("Waveform matches multiple candidate units");
    } else {
        result.reason = QStringLiteral("Waveform matches no candidate unit");
    }
    return result;
}

} // namespace ccv2
