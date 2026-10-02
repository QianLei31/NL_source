#pragma once

#include "signal/spike_event.h"
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

namespace ccv2 {

// Owns immutable waveform data. Never contains a pointer into a display ring.
struct SpikeArchiveRecord {
    SpikeEvent event;
    QVector<float> waveform;
};

enum class SpikeArchiveState { Idle, Running, Completed, Drained, Cancelled, Failed };
enum class SpikeArchiveEnqueueMode { LiveNonBlocking, OfflineBlocking };
enum class SpikeArchiveEnqueueResult { Accepted, Cancelled, Failed, NotRunning };

struct SpikeArchiveOptions {
    qint64 maxQueuedBytes = 32 * 1024 * 1024;
    // Deterministic fault injection for tests. Zero/-1 disable respectively.
    int testWriteDelayMs = 0;
    qint64 testFailAfterBytes = -1;
};

struct SpikeArchiveStatus {
    SpikeArchiveState state = SpikeArchiveState::Idle;
    QUuid runId;
    quint64 acceptedEvents = 0;
    quint64 writtenEvents = 0;
    quint64 lastEventId = 0;
    qint64 queuedBytes = 0; // includes the writer's in-flight record
    qint64 peakQueuedBytes = 0;
    qint64 verifiedBytes = 0;
    bool recoveredPrefix = false;
    bool finishVerified = false;
    // Writer-owned thread termination, distinct from an early Failed state.
    // Reader snapshots do not establish whether another process owns a writer.
    bool writerFinished = false;
    QString error;
    // Source gaps/invalid samples are independent of the archive lifecycle.
    QJsonObject sourceIntegrity;
};

struct SpikeArchiveAnnotation;

class SpikeEventArchiveWriter {
public:
    SpikeEventArchiveWriter();
    ~SpikeEventArchiveWriter();
    SpikeEventArchiveWriter(const SpikeEventArchiveWriter &) = delete;
    SpikeEventArchiveWriter &operator=(const SpikeEventArchiveWriter &) = delete;

    // Creates only a NEW sidecar directory, never changes raw files/session.json.
    bool begin(const QString &directory, const QUuid &runId,
               const QJsonObject &runMetadata, const SpikeArchiveOptions &options = {},
               QString *error = nullptr);
    // Revision metadata must describe the actual complete applied configuration
    // and saved rule set. Enqueue before events using that revision pair.
    SpikeArchiveEnqueueResult enqueueRevision(quint64 detectorRevision, quint64 ruleRevision,
        const QJsonObject &metadata, SpikeArchiveEnqueueMode mode = SpikeArchiveEnqueueMode::OfflineBlocking,
        const std::atomic_bool *cancel = nullptr);
    // IDs must be nonzero, strictly increasing within runId, and already assigned
    // by the producer before its independent display-ring insertion.
    SpikeArchiveEnqueueResult enqueueBatch(const QVector<SpikeArchiveRecord> &events,
        SpikeArchiveEnqueueMode mode, const std::atomic_bool *cancel = nullptr);
    // Active-run labels share the ordered event queue. Up to 2048 labels per call;
    // input annotationId/utc are replaced with archive-assigned values.
    SpikeArchiveEnqueueResult enqueueManualLabels(const QVector<SpikeArchiveAnnotation> &labels,
        SpikeArchiveEnqueueMode mode = SpikeArchiveEnqueueMode::OfflineBlocking,
        const std::atomic_bool *cancel = nullptr);
    // All accepted records are drained, including on cancellation. Failed writes
    // or live queue exhaustion always produce Failed, never successful truncation.
    // Explicit Failed uses sourceIntegrity["error"] as its reason and drains the
    // accepted prefix without a successful terminal record/finish marker.
    bool finish(SpikeArchiveState terminal, const QJsonObject &sourceIntegrity = {});
    void wait();
    SpikeArchiveStatus status() const;
    QString directory() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

struct SpikeArchiveQuery {
    int electrode = -1; // -1 = any
    bool filterEpoch = false;
    quint64 epoch = 0;
    double sourceTimeFrom = -1.0; // inclusive, negative = unbounded
    double sourceTimeTo = -1.0;   // inclusive
};

struct SpikeArchiveCursor {
    qint64 byteOffset = 32;
    quint32 itemOffset = 0;
};

struct SpikeArchivePage {
    QVector<SpikeArchiveRecord> events;
    SpikeArchiveCursor next;
    bool atEnd = false;
    QString error;
};

struct SpikeArchiveAnnotation {
    QUuid runId;
    quint64 eventId = 0;
    int unitId = -1;
    QString note;
    QString utc;
    quint64 annotationId = 0;
};

struct SpikeArchiveAnnotationPage {
    QVector<SpikeArchiveAnnotation> annotations;
    qint64 nextOffset = 32;
    bool atEnd = false;
    bool recoveredPrefix = false;
    QString error;
};

class SpikeEventArchiveReader {
public:
    // Read-only verified-prefix recovery: never repairs/truncates original files.
    bool open(const QString &directory, QString *error = nullptr,
              const std::atomic_bool *cancel = nullptr);
    SpikeArchiveStatus status() const { return m_status; }
    QJsonObject runMetadata() const { return m_metadata; }
    QString directory() const { return m_directory; }
    // Memory bounded by <=256 returned events and <=16 MiB one decoded frame.
    // scanBudgetBytes bounds work per call even when no events match a query.
    SpikeArchivePage readPage(const SpikeArchiveQuery &query = {},
        SpikeArchiveCursor cursor = {}, int limit = 128,
        qint64 scanBudgetBytes = 64 * 1024 * 1024) const;
    bool revisionMetadata(quint64 detectorRevision, quint64 ruleRevision,
                          QJsonObject *metadata, QString *error = nullptr) const;
    // Negative continuation offsets address the post-close annotation sidecar.
    SpikeArchiveAnnotationPage readAnnotations(quint64 eventId = 0,
        qint64 offset = 32, int limit = 128) const;
    // Serialized by a sidecar lock, append-only; original automated assignment
    // and waveform remain immutable. Event must exist in the verified prefix.
    static bool appendManualLabels(const QString &directory, const QVector<SpikeArchiveAnnotation> &labels,
        QString *error = nullptr, const std::atomic_bool *cancel = nullptr);
    static bool appendManualLabel(const QString &directory, const QUuid &runId,
        quint64 eventId, int unitId, const QString &note = {}, QString *error = nullptr,
        const std::atomic_bool *cancel = nullptr);
private:
    QString m_directory;
    SpikeArchiveStatus m_status;
    QJsonObject m_metadata;
    qint64 m_annotationVerifiedBytes = 32;
    QString m_annotationRecoveryError;
};

QString spikeArchiveStateName(SpikeArchiveState state);

} // namespace ccv2
