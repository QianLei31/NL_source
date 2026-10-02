#include "spike_offline_analysis.h"

#include "core/channel_routing.h"
#include "core/constants.h"
#include "network/replay_controller.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>

namespace ccv2 {
namespace {
bool validConfiguration(const SpikeOfflineAnalysisRequest &r,const SpikeDetectConfig &c,QString *error) {
    const qint64 snippet=qint64(c.preSamples)+c.postSamples+1;
    if(r.inputPath.isEmpty()||r.outputDirectory.isEmpty()||r.blockFrames<1||r.blockFrames>4096||
       r.queueBlocks<1||r.queueBlocks>64||r.blockFrames*r.queueBlocks*kFrameBytes>64LL*1024*1024||
       r.displayRetentionPerLane<1||r.displayRetentionPerLane>4096||r.testReadDelayMs<0||
       c.preSamples<1||c.postSamples<1||snippet>SpikeRuleSet::kMaxSnippetSamples||
       !std::isfinite(c.proc.sampleRate)||c.proc.sampleRate<1||c.proc.sampleRate>1e9||
       !std::isfinite(c.proc.lowpassHz)||c.proc.lowpassHz<0||c.proc.lowpassHz>1e9){
        *error=QStringLiteral("Invalid offline input, bounded queue, retention, sample rate, or waveform configuration");return false;
    }
    SpikeRuleContext ctx;ctx.adcChannel=0;ctx.electrode=0;ctx.sourceSampleRate=c.proc.sampleRate*(c.tdmEnabled?4:1);
    ctx.inputGain=c.inputGain;ctx.referenceMode=r.referenceMode;ctx.filterOrder=c.proc.filterOrder;ctx.notchHz=c.proc.notchHz;
    ctx.highpassHz=c.proc.highpassHz;ctx.spikeLowpassHz=c.proc.spikeLowpassHz;ctx.absoluteThreshold=c.proc.absoluteThreshold;
    ctx.absThresholdV=c.proc.absThresholdV;ctx.rmsMultiple=c.proc.rmsMultiple;ctx.negativePolarity=c.proc.negativePolarity;
    ctx.refractoryMs=c.proc.refractoryMs;ctx.preSamples=c.preSamples;ctx.postSamples=c.postSamples;
    if(!SpikeRuleSet::validateContext(ctx,error))return false;
    for(auto it=r.thresholdOverridesUvByElectrode.cbegin();it!=r.thresholdOverridesUvByElectrode.cend();++it){
        if(it.key()<0||it.key()>1023||!std::isfinite(it.value())||it.value()<=0||it.value()>1e9){
            *error=QStringLiteral("Invalid physical-electrode threshold override");return false;
        }
    }
    return true;
}
QJsonObject qualityJson(const SpikeAnalysisQuality &q,const QJsonObject &source,qint64 total,qint64 read,qint64 processed,bool eof,const QString &error) {
    QJsonObject o=source;
    o["epoch"]=QString::number(q.epoch);o["total_file_frames"]=QString::number(total);o["read_file_frames"]=QString::number(read);
    o["processed_file_frames"]=QString::number(processed);o["input_eof"]=eof;
    o["missing_source_frames"]=QString::number(q.missingSourceFrames);o["queue_dropped_frames"]=QString::number(q.queueDroppedFrames);
    o["invalid_frames"]=QString::number(q.invalidFrames);o["pending_window_events"]=QString::number(q.pendingWindowEvents);
    o["boundary_excluded_events"]=QString::number(q.boundaryExcludedEvents);o["unverified_frames"]=QString::number(q.unverifiedFrames);
    o["discontinuities"]=QString::number(q.discontinuities);o["stopped_early"]=q.stoppedEarly;
    o["first_source_frame"]=QString::number(q.firstSourceFrame);o["last_source_frame"]=QString::number(q.lastSourceFrame);
    const bool verifiedCoverage=eof&&processed==total&&!q.incomplete()&&source.value("integrity_complete").toBool();
    o["incomplete_analysis_coverage"]=!verifiedCoverage;o["incomplete_coverage"]=!verifiedCoverage;
    o["archive_scope"]="all_input_frames_lossless_offline";
    o["whole_source_coverage_claimed"]=verifiedCoverage;
    if(!error.isEmpty())o["error"]=error;
    return o;
}
}

SpikeOfflineAnalysisJob::SpikeOfflineAnalysisJob(const SpikeOfflineAnalysisRequest &request,QObject *parent)
    :QThread(parent),m_request(request) {
    m_status.runId=QUuid::createUuid();m_status.outputDirectory=QDir(request.outputDirectory).absolutePath();
}
SpikeOfflineAnalysisJob::~SpikeOfflineAnalysisJob(){requestCancel();wait();}
void SpikeOfflineAnalysisJob::requestCancel(){
    QMutexLocker lock(&m_statusMutex);
    if(m_status.cancellable)m_cancel.store(true,std::memory_order_release);
}
SpikeOfflineAnalysisStatus SpikeOfflineAnalysisJob::status() const {QMutexLocker lock(&m_statusMutex);return m_status;}

void SpikeOfflineAnalysisJob::run() {
    if(m_started.exchange(true))return;
    auto earlyFailure=[&](const QString &error){
        {QMutexLocker lock(&m_statusMutex);m_status.state=SpikeArchiveState::Failed;m_status.error=error;m_status.cancellable=false;}
        emit progressChanged();
    };
    SpikeDetectConfig cfg=m_request.config;QString error;
    if(!validConfiguration(m_request,cfg,&error)){earlyFailure(error);return;}
    if(m_cancel.load()){
        {QMutexLocker lock(&m_statusMutex);m_status.state=SpikeArchiveState::Cancelled;m_status.cancellable=false;}
        emit progressChanged();return;
    }
    auto queue=std::make_shared<ThreadSafeQueue<QByteArray>>(m_request.queueBlocks);
    // QObject and its file/timer are constructed, used and destroyed on this
    // dedicated job thread. No timed replay or GUI event loop is involved.
    ReplayController source(queue);
    if(!source.open(m_request.inputPath,cfg.proc.sampleRate*(cfg.tdmEnabled?4:1))){earlyFailure(source.errorString());return;}
    source.setTimelineEpoch(m_request.epoch);
    if(source.tdmKnown()){cfg.tdmEnabled=source.tdmEnabled();cfg.tdmPair02=source.tdmEvenFirst();}
    cfg.proc.sampleRate=source.sampleRate()/(cfg.tdmEnabled?4:1);
    cfg.proc.band=SpikeProcConfig::SpikeBand;
    if(!validConfiguration(m_request,cfg,&error)){earlyFailure(error);return;}
    SpikeSnippetStore display;
    if(!display.configure(cfg.tdmEnabled?512:256,cfg.preSamples+cfg.postSamples+1,m_request.displayRetentionPerLane)){
        earlyFailure(QStringLiteral("Cannot allocate bounded offline display retention"));return;
    }
    display.resetTimeline(m_request.epoch);
    const auto runId=status().runId;
    QJsonObject metadata{{"analysis_mode","dedicated_lossless_offline"},{"input_path",QFileInfo(m_request.inputPath).absoluteFilePath()},
        {"created_at_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"source",source.sourceProvenance()},{"source_sample_rate_hz",source.sampleRate()},
        {"source_frame_origin",QString::number(source.metadata().frameOrigin)},
        {"tdm_known",source.tdmKnown()},{"tdm_enabled",cfg.tdmEnabled},{"tdm_pair_02",cfg.tdmPair02},
        {"raw_fallback_declared_source_rate_hz",m_request.config.proc.sampleRate*(m_request.config.tdmEnabled?4:1)},
        {"original_recording_analysis",source.metadata().neuralAnalysis},
        {"queue_blocks",m_request.queueBlocks},{"block_frames",QString::number(m_request.blockFrames)},
        {"display_retention_per_lane",display.capacity()},
        {"analysis_scope",cfg.tdmEnabled?"selected_512_of_1024_electrodes":"all_256_adc_channels"},
        {"archive_scope","all_input_frames_lossless_offline"}};
    SpikeEventArchiveWriter writer;
    if(!writer.begin(m_request.outputDirectory,runId,metadata,m_request.archiveOptions,&error)){earlyFailure(error);return;}
    {QMutexLocker lock(&m_statusMutex);m_status.state=SpikeArchiveState::Running;m_status.totalFrames=source.totalFrames();
        m_status.sourceSampleRate=source.sampleRate();m_status.tdmKnown=source.tdmKnown();m_status.tdmEnabled=cfg.tdmEnabled;
        m_status.tdmPair02=cfg.tdmPair02;m_status.warning=source.warning();m_status.archive=writer.status();}
    emit progressChanged();
    std::atomic_bool detectorStop{false};std::atomic<quint64> epoch{m_request.epoch};std::atomic<int> reference{m_request.referenceMode};
    std::atomic<qint64> processed{0};QMutex failureMutex;QString processingError;
    auto fail=[&](const QString &message){
        {QMutexLocker lock(&failureMutex);if(processingError.isEmpty())processingError=message;}
        detectorStop.store(true);queue->wakeAll();
    };
    auto currentError=[&]{QMutexLocker lock(&failureMutex);return processingError;};
    QElapsedTimer progressClock;progressClock.start();std::atomic<qint64> lastProgress{-100};
    auto publish=[&]{
        const auto archive=writer.status();
        {QMutexLocker lock(&m_statusMutex);m_status.archive=archive;m_status.processedFrames=processed.load();}
        // Offline processing can greatly exceed real-time. Bound queued GUI
        // notifications too; observers read the latest thread-safe snapshot.
        const qint64 now=progressClock.elapsed();qint64 previous=lastProgress.load();
        if(now-previous>=50&&lastProgress.compare_exchange_strong(previous,now))emit progressChanged();
    };
    SpikeDetectWorker detector(queue,&display,&detectorStop,cfg,&epoch,&reference,source.nextSourceFrame());
    QObject::connect(&detector,&SpikeDetectWorker::framesProcessed,&detector,[&](qint64 frames){processed.fetch_add(frames);publish();},Qt::DirectConnection);
    const bool callbacks=detector.setEventCallbacks(runId,
        [&](quint64 detectorRevision,quint64 ruleRevision,const QJsonObject &actualMetadata){
            const auto result=writer.enqueueRevision(detectorRevision,ruleRevision,actualMetadata,SpikeArchiveEnqueueMode::OfflineBlocking,&m_cancel);
            if(result!=SpikeArchiveEnqueueResult::Accepted){if(result==SpikeArchiveEnqueueResult::Cancelled)detectorStop.store(true);
                else fail(writer.status().error.isEmpty()?QStringLiteral("Archive rejected detector revision metadata"):writer.status().error);}
        },
        [&](const QVector<SpikeArchiveRecord> &batch){
            const auto result=writer.enqueueBatch(batch,SpikeArchiveEnqueueMode::OfflineBlocking,&m_cancel);
            if(result!=SpikeArchiveEnqueueResult::Accepted){if(result==SpikeArchiveEnqueueResult::Cancelled)detectorStop.store(true);
                else fail(writer.status().error.isEmpty()?QStringLiteral("Archive rejected immutable detector batch"):writer.status().error);}
        });
    if(!callbacks||(m_request.rules&&!detector.setRuleSet(m_request.rules)))fail(QStringLiteral("Cannot configure immutable offline detector callbacks/rules"));
    for(int lane=0;lane<detector.laneCount();++lane){
        const int electrode=cfg.tdmEnabled?(lane/2)*4+(lane%2?tdmLocalElePair(cfg.tdmPair02).second:tdmLocalElePair(cfg.tdmPair02).first):lane;
        auto it=m_request.thresholdOverridesUvByElectrode.constFind(electrode);
        if(it!=m_request.thresholdOverridesUvByElectrode.cend())detector.setLaneThresholdOverride(lane,true,(cfg.proc.negativePolarity?-1:1)*it.value()*1e-6);
    }
    bool eof=false;
    qint64 readFrames=0,backpressure=0;
    if(!detectorStop.load())detector.start();
    while(!m_cancel.load()&&!detectorStop.load()){
        if(writer.status().state==SpikeArchiveState::Failed){fail(writer.status().error);break;}
        if(m_request.testReadFailureAfterFrames>=0&&readFrames>=m_request.testReadFailureAfterFrames){fail(QStringLiteral("Injected offline input-read failure"));break;}
        QByteArray bytes;StreamBlockInfo info;
        if(!source.readNextBlock(m_request.blockFrames,&bytes,&info)){fail(source.errorString());break;}
        if(bytes.isEmpty()){eof=true;break;}
        const qint64 count=bytes.size()/kFrameBytes;readFrames+=count;
        {QMutexLocker lock(&m_statusMutex);m_status.readFrames=readFrames;}
        while(!queue->push(bytes,false,nullptr,info)){
            ++backpressure;
            if(m_cancel.load()||detectorStop.load())break;
            if(writer.status().state==SpikeArchiveState::Failed){fail(writer.status().error);break;}
            if(!detector.isRunning()){fail(QStringLiteral("Offline detector stopped before input EOF"));break;}
            QThread::msleep(1);
        }
        if(m_cancel.load()||detectorStop.load())break;
        {QMutexLocker lock(&m_statusMutex);m_status.backpressureWaits=backpressure;}
        publish();
        if(m_request.testReadDelayMs>0){QElapsedTimer delay;delay.start();
            while(delay.elapsed()<m_request.testReadDelayMs&&!m_cancel.load()&&!detectorStop.load())QThread::msleep(1);}
    }
    {QMutexLocker lock(&m_statusMutex);m_status.inputEof=eof;m_status.draining=true;}
    if(eof&&!m_cancel.load()&&currentError().isEmpty())detector.requestDrain();
    else {detectorStop.store(true);queue->wakeAll();}
    while(!detector.wait(20)){
        if(writer.status().state==SpikeArchiveState::Failed)fail(writer.status().error);
        if(m_cancel.load()){detectorStop.store(true);queue->wakeAll();}
        publish();
    }
    if(eof&&!m_cancel.load()&&currentError().isEmpty()&&!detector.drained())fail(QStringLiteral("Offline detector failed to drain every input block"));
    if(eof&&!m_cancel.load()&&currentError().isEmpty()&&processed.load()!=source.totalFrames())fail(QStringLiteral("Offline processed-frame count differs from complete input"));
    if(!eof||m_cancel.load()||!currentError().isEmpty())display.markAnalysisIncomplete(m_request.epoch);
    SpikeAnalysisQuality quality;QVector<qint64> totals;display.snapshotAnalysis(nullptr,&quality,&totals);
    qint64 detected=0,retained=0;for(qint64 n:totals){detected+=n;retained+=qMin<qint64>(n,display.capacity());}
    error=currentError();
    auto archive=writer.status();if(archive.state==SpikeArchiveState::Failed&&error.isEmpty())error=archive.error;
    auto integrity=qualityJson(quality,source.sourceProvenance(),source.totalFrames(),readFrames,processed.load(),eof,error);
    bool cancelledBeforeCommit=false;
    {QMutexLocker lock(&m_statusMutex);m_status.cancellable=false;m_status.finalizing=true;cancelledBeforeCommit=m_cancel.load();}
    emit progressChanged();
    if(cancelledBeforeCommit){quality.stoppedEarly=true;integrity=qualityJson(quality,source.sourceProvenance(),source.totalFrames(),readFrames,processed.load(),eof,error);}
    const auto terminal=!error.isEmpty()?SpikeArchiveState::Failed:(cancelledBeforeCommit?SpikeArchiveState::Cancelled:SpikeArchiveState::Completed);
    if(!writer.finish(terminal,integrity)&&writer.status().state==SpikeArchiveState::Running){
        error=QStringLiteral("Cannot finalize offline archive integrity metadata");integrity["error"]=error;
        writer.finish(SpikeArchiveState::Failed,{{"error",error}});
    }
    writer.wait();archive=writer.status();
    if(archive.state==SpikeArchiveState::Failed&&error.isEmpty())error=archive.error;
    {QMutexLocker lock(&m_statusMutex);m_status.archive=archive;m_status.state=archive.state;m_status.error=error;
        m_status.processedFrames=processed.load();m_status.readFrames=readFrames;m_status.retainedEvents=retained;m_status.detectedEvents=detected;
        m_status.sourceIntegrity=integrity;m_status.inputEof=eof;m_status.draining=false;m_status.finalizing=false;m_status.cancellable=false;m_status.backpressureWaits=backpressure;}
    emit progressChanged();
}

} // namespace ccv2
