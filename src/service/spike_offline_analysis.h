#pragma once

#include "io/spike_event_archive.h"
#include "service/spike_detect_worker.h"
#include "signal/spike_rule_classifier.h"
#include <QMap>
#include <QMutex>
#include <QThread>
#include <atomic>

namespace ccv2 {

struct SpikeOfflineAnalysisRequest {
    QString inputPath; // manifest, session directory, or one raw BIN
    QString outputDirectory; // NEW/empty sidecar directory, never raw/session.json
    SpikeDetectConfig config; // proc.sampleRate is lane fs; raw fallback source fs is lane fs * stride
    int referenceMode = 0;
    QMap<int,double> thresholdOverridesUvByElectrode; // positive magnitudes
    SpikeRuleSet::Snapshot rules;
    SpikeArchiveOptions archiveOptions;
    qint64 blockFrames = 1024;
    int queueBlocks = 4;
    int displayRetentionPerLane = 32;
    quint64 epoch = 1;
    // Deterministic testing hooks, inactive in normal requests.
    int testReadDelayMs = 0;
    qint64 testReadFailureAfterFrames = -1;
};

struct SpikeOfflineAnalysisStatus {
    SpikeArchiveState state = SpikeArchiveState::Idle;
    SpikeArchiveStatus archive;
    QUuid runId;
    QString outputDirectory;
    QString error;
    QString warning;
    qint64 totalFrames = 0;
    qint64 readFrames = 0;
    qint64 processedFrames = 0;
    qint64 retainedEvents = 0;
    qint64 detectedEvents = 0;
    qint64 backpressureWaits = 0;
    bool inputEof = false;
    bool draining = false;
    bool finalizing = false;
    bool cancellable = true; // false once accepted-tail terminal commit begins
    double sourceSampleRate = 0.0;
    bool tdmKnown = false;
    bool tdmEnabled = false;
    bool tdmPair02 = true;
    QJsonObject sourceIntegrity;
};

// Dedicated full-file pull -> bounded no-drop detector queue -> immutable archive.
// No SessionHub subscription or display refresh drives this job. The constructor
// freezes the request. Start once; requestCancel() is safe from any thread.
class SpikeOfflineAnalysisJob : public QThread {
    Q_OBJECT
public:
    explicit SpikeOfflineAnalysisJob(const SpikeOfflineAnalysisRequest &request,
                                    QObject *parent = nullptr);
    ~SpikeOfflineAnalysisJob() override;
    void requestCancel();
    SpikeOfflineAnalysisStatus status() const;
signals:
    void progressChanged();
protected:
    void run() override;
private:
    const SpikeOfflineAnalysisRequest m_request;
    mutable QMutex m_statusMutex;
    SpikeOfflineAnalysisStatus m_status;
    std::atomic_bool m_cancel{false};
    std::atomic_bool m_started{false};
};

} // namespace ccv2
