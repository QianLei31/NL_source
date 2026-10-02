#pragma once
#include "io/spike_event_archive.h"
#include <QMap>
#include <QSet>
#include <QMutex>
#include <memory>
namespace ccv2 {
class SpikeDetectWorker;
class SpikeSnippetStore;
// Serializes live detector callbacks with explicit archive start/stop boundaries.
// This controller never reads the display ring to obtain automated events.
class NeuralArchiveController {
public:
    ~NeuralArchiveController();
    void attach(SpikeDetectWorker *worker);
    bool start(const QString &directory, const QJsonObject &metadata, QString *error = nullptr,
               const SpikeArchiveOptions &options = {});
    void finish(SpikeArchiveState state, const QJsonObject &integrity = {}, bool waitForWriter = true);
    SpikeArchiveStatus status() const;
    QString directory() const;
    void annotate(const QVector<SpikeEvent> &events);
private:
    void revision(quint64 detector, quint64 rule, const QJsonObject &metadata);
    void events(const QVector<SpikeArchiveRecord> &records);
    mutable QMutex m_mutex;
    QUuid m_runId;
    QMap<QPair<quint64,quint64>,QJsonObject> m_revisions;
    std::unique_ptr<SpikeEventArchiveWriter> m_writer;
    qint64 m_revisionBytes = 0;
    bool m_revisionOverflow = false;
    QPair<quint64,quint64> m_latestRevision;
    QSet<QPair<quint64,quint64>> m_registered;
    quint64 m_firstArchivedId = 0;
    quint64 m_lastArchivedId = 0;
    bool m_closing = false;
    QString m_annotationError;
    struct AnnotationTask;
    std::shared_ptr<AnnotationTask> m_annotationTask;
};
QJsonObject spikeQualityJson(const SpikeSnippetStore &store);
}
