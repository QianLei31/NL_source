#include "service/neural_archive_controller.h"
#include "service/spike_detect_worker.h"
#include "signal/spike_snippet_store.h"
#include <QMutexLocker>
#include <QJsonDocument>
#include <QtConcurrent>
namespace ccv2 {
struct NeuralArchiveController::AnnotationTask {
    std::atomic_bool cancel{false};
    std::atomic_bool running{true};
    QMutex mutex;
    QString error;
};
NeuralArchiveController::~NeuralArchiveController() {
    if (m_annotationTask) m_annotationTask->cancel.store(true);
}

void NeuralArchiveController::attach(SpikeDetectWorker *worker) {
    finish(SpikeArchiveState::Drained, {{"end_reason", "detector_replaced"}});
    QMutexLocker lock(&m_mutex);
    m_revisions.clear(); m_revisionBytes=0; m_revisionOverflow=false; m_latestRevision={}; m_runId = worker->runId();
    worker->setEventCallbacks(m_runId,
        [this](quint64 d, quint64 r, const QJsonObject &m) { revision(d,r,m); },
        [this](const QVector<SpikeArchiveRecord> &e) { events(e); });
}
bool NeuralArchiveController::start(const QString &directory, const QJsonObject &metadata,
                                    QString *error, const SpikeArchiveOptions &options) {
    QMutexLocker lock(&m_mutex);
    const auto prior = m_writer ? m_writer->status() : SpikeArchiveStatus{};
    if (m_writer && !prior.writerFinished) {
        if (error) *error=QStringLiteral("上一档案仍在排空已接收尾部，请稍后重试"); return false;
    }
    if (m_revisionOverflow) { if (error) *error=QStringLiteral("配置历史已达内存上限，请重启检测后建立新档案"); return false; }
    if (m_runId.isNull() || (m_writer && prior.state == SpikeArchiveState::Running)) {
        if (error) *error = QStringLiteral("请先运行检测，并先结束已有事件档案"); return false;
    }
    auto writer = std::make_unique<SpikeEventArchiveWriter>();
    QJsonObject run = metadata;
    run["event_scope"] = "immutable_complete_windows_emitted_after_archive_start";
    run["delivery_policy"] = "live_nonblocking_explicit_failure_on_archive_overflow";
    if (!writer->begin(directory, m_runId, run, options, error)) return false;
    m_writer = std::move(writer); m_firstArchivedId = m_lastArchivedId = 0;
    m_annotationError.clear(); m_registered.clear(); m_closing=false;
    if (m_revisions.contains(m_latestRevision)) {
        if (m_writer->enqueueRevision(m_latestRevision.first,m_latestRevision.second,
            m_revisions.value(m_latestRevision),SpikeArchiveEnqueueMode::LiveNonBlocking) == SpikeArchiveEnqueueResult::Accepted)
            m_registered.insert(m_latestRevision);
    }
    const auto state=m_writer->status();
    if (state.state != SpikeArchiveState::Running && error) *error=state.error;
    return state.state == SpikeArchiveState::Running;
}
void NeuralArchiveController::revision(quint64 detector, quint64 rule, const QJsonObject &metadata) {
    QMutexLocker lock(&m_mutex);
    // A configured worker emits each pair once. Retain metadata for an archive
    // started later in that worker's lifetime, but bound hostile edit churn.
    const auto key=qMakePair(detector,rule);
    if (!m_revisions.contains(key)) {
        const qint64 bytes=QJsonDocument(metadata).toJson(QJsonDocument::Compact).size();
        if (m_revisions.size() >= 65536 || m_revisionBytes + bytes > 32*1024*1024) {
            m_revisionOverflow=true;
            if (m_writer && m_writer->status().state == SpikeArchiveState::Running)
                m_writer->finish(SpikeArchiveState::Failed,{{"error","applied revision history exceeds 32 MiB bound"}});
            return;
        }
        m_revisionBytes += bytes;
        m_revisions.insert(key,metadata);
    }
    m_latestRevision=key;
    if (m_writer && m_writer->status().state == SpikeArchiveState::Running && !m_closing && !m_registered.contains(key)) {
        if (m_writer->enqueueRevision(detector,rule,metadata,SpikeArchiveEnqueueMode::LiveNonBlocking) == SpikeArchiveEnqueueResult::Accepted)
            m_registered.insert(key);
    }
}
void NeuralArchiveController::events(const QVector<SpikeArchiveRecord> &records) {
    if (records.isEmpty()) return;
    QMutexLocker lock(&m_mutex);
    if (!m_writer || m_closing || m_writer->status().state != SpikeArchiveState::Running) return;
    for (const auto &record : records) {
        const auto key = qMakePair(record.event.detectorRevision,record.event.ruleRevision);
        if (m_registered.contains(key)) continue;
        const auto it = m_revisions.constFind(key);
        if (it == m_revisions.cend()) {
            m_writer->finish(SpikeArchiveState::Failed, {{"error","missing applied revision metadata"}}); return;
        }
        if (m_writer->enqueueRevision(key.first,key.second,*it,SpikeArchiveEnqueueMode::LiveNonBlocking)
                != SpikeArchiveEnqueueResult::Accepted) return;
        m_registered.insert(key);
    }
    if (m_writer->enqueueBatch(records,SpikeArchiveEnqueueMode::LiveNonBlocking)
            == SpikeArchiveEnqueueResult::Accepted) {
        if (!m_firstArchivedId) m_firstArchivedId = records.first().event.eventId;
        m_lastArchivedId = records.last().event.eventId;
    }
}
void NeuralArchiveController::finish(SpikeArchiveState state, const QJsonObject &integrity, bool waitForWriter) {
    SpikeEventArchiveWriter *writer=nullptr;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_writer) return;
        writer=m_writer.get();
        if (!m_closing) {
            m_closing=true;
            QJsonObject scoped=integrity;
            scoped["archive_scope"]="complete_window_emission_interval_after_explicit_start";
            scoped["whole_source_coverage_claimed"]=false;
            if (writer->status().state == SpikeArchiveState::Running && !writer->finish(state,scoped))
                writer->finish(SpikeArchiveState::Failed,{{"error","archive finalization metadata rejected"}});
        }
    }
    // All producer callbacks now skip this writer without waiting for its disk
    // drain. GUI start/attach are serialized on their owning thread, so this
    // writer remains alive throughout a requested synchronous shutdown join.
    if (waitForWriter) writer->wait();
}
SpikeArchiveStatus NeuralArchiveController::status() const {
    QMutexLocker lock(&m_mutex);
    SpikeArchiveStatus s = m_writer ? m_writer->status() : SpikeArchiveStatus{};
    if (!m_annotationError.isEmpty()) s.error += QStringLiteral(" 标签保存: ") + m_annotationError;
    if (m_annotationTask) {
        QMutexLocker taskLock(&m_annotationTask->mutex);
        if (!m_annotationTask->error.isEmpty()) s.error += QStringLiteral(" 标签保存: ")+m_annotationTask->error;
        else if (m_annotationTask->running.load()) s.error += QStringLiteral(" 人工标签正在写入历史日志");
    }
    return s;
}
QString NeuralArchiveController::directory() const {
    QMutexLocker lock(&m_mutex); return m_writer ? m_writer->directory() : QString{};
}
void NeuralArchiveController::annotate(const QVector<SpikeEvent> &events) {
    QMutexLocker lock(&m_mutex);
    if (!m_writer) return;
    QVector<SpikeArchiveAnnotation> labels;
    const auto s = m_writer->status();
    for (const auto &e : events) {
        if (e.runId != s.runId || !m_firstArchivedId || e.eventId < m_firstArchivedId || e.eventId > m_lastArchivedId) continue;
        labels.push_back({e.runId,e.eventId,e.unitId,QStringLiteral("retained candidate workspace")});
    }
    if (labels.isEmpty()) return;
    m_annotationError.clear(); // a new accepted attempt replaces any stale retry notice
    if (s.state == SpikeArchiveState::Running && !m_closing) {
        if (m_writer->enqueueManualLabels(labels,SpikeArchiveEnqueueMode::LiveNonBlocking)
                != SpikeArchiveEnqueueResult::Accepted) m_annotationError = QStringLiteral("活动档案未接受人工标签");
    } else {
        if (s.state == SpikeArchiveState::Running) {
            m_annotationError=QStringLiteral("档案正在排空；本批仅更新留存窗口，请完成后重试保存标签"); return;
        }
        if (m_annotationTask && m_annotationTask->running.load()) {
            m_annotationError=QStringLiteral("上一批历史标签仍在写入；本批仅更新留存窗口，请稍后重试"); return;
        }
        m_annotationTask=std::make_shared<AnnotationTask>();
        const auto task=m_annotationTask;
        const QString path=m_writer->directory();
        auto future=QtConcurrent::run([task,path,labels] {
            QString error;
            SpikeEventArchiveReader::appendManualLabels(path,labels,&error,&task->cancel);
            {QMutexLocker taskLock(&task->mutex);task->error=error;}
            task->running.store(false);
        });
        Q_UNUSED(future);
    }
}
QJsonObject spikeQualityJson(const SpikeSnippetStore &store) {
    SpikeAnalysisQuality q; store.snapshotAnalysis(nullptr,&q);
    return {{"epoch",QString::number(q.epoch)}, {"missing_source_frames",QString::number(q.missingSourceFrames)},
        {"queue_dropped_frames",QString::number(q.queueDroppedFrames)}, {"invalid_frames",QString::number(q.invalidFrames)},
        {"unverified_frames",QString::number(q.unverifiedFrames)}, {"discontinuities",QString::number(q.discontinuities)},
        {"pending_window_events",QString::number(q.pendingWindowEvents)}, {"boundary_excluded_events",QString::number(q.boundaryExcludedEvents)},
        {"stopped_early",q.stoppedEarly}, {"first_source_frame",QString::number(q.firstSourceFrame)},
        {"last_source_frame",QString::number(q.lastSourceFrame)}, {"incomplete_coverage",q.incomplete()}};
}
}
