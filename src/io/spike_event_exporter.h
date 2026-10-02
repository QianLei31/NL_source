#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include "service/spike_detect_worker.h"
#include "signal/spike_snippet_store.h"

namespace ccv2 {

// Versioned provenance for the software analysis configuration. Gain is a
// manually selected uniform conversion; it is not a verified hardware readback.
QJsonObject spikeAnalysisMetadata(const SpikeDetectConfig &config, int referenceMode,
                                  const QMap<int, double> &thresholdOverridesUv = {});

// One self-contained, atomically committed selected-lane retained-window file.
// This is not a complete event recording: evicted/uncaptured spikes are disclosed.
bool exportSpikeLaneJson(const QString &path, const SpikeLaneSnapshot &snapshot,
                         const QJsonObject &analysisMetadata, QString *error = nullptr);

} // namespace ccv2
