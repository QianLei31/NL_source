#include "service/spike_offline_analysis.h"
#include "service/dummy_stream_server.h"
#include "io/session_manifest.h"
#include "core/constants.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>

using namespace ccv2;
#define CHECK(c) do {if(!(c)){std::cerr<<"FAIL line "<<__LINE__<<": " #c "\n";return false;}}while(false)
static bool until(const std::function<bool()> &test,int ms=10000) {
    QElapsedTimer timer;timer.start();do{QCoreApplication::processEvents(QEventLoop::AllEvents,5);if(test())return true;QThread::msleep(1);}while(timer.elapsed()<ms);return test();
}
static bool write(const QString &path,const QByteArray &bytes){QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();}
static QByteArray hashFile(const QString &path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(&f);return hash.result();}
static SpikeDetectConfig config(){SpikeDetectConfig c;c.inputGain=60;c.proc.sampleRate=20000;c.proc.notchHz=0;c.proc.highpassHz=250;c.proc.spikeLowpassHz=6000;
    c.proc.absoluteThreshold=true;c.proc.absThresholdV=-100e-6;c.proc.negativePolarity=true;c.proc.refractoryMs=5;c.preSamples=8;c.postSamples=256;return c;}
static bool fixture(const QString &path,bool tdm=false){
    CHECK(QDir().mkpath(path));constexpr int frames=12000,split=6100;const qint64 origin=tdm?4003:1200;
    QByteArray bytes(frames*kFrameBytes,Qt::Uninitialized);
    for(int f=0;f<frames;++f){const qint64 source=origin+f+(f>=6000?104:0);
        for(int ch=0;ch<256;++ch){const bool pulse=(f>=800&&(f-800)%240<24)||f>=11990;
            const int code=2048-((ch==7&&pulse&&(!tdm||source%4==3))?90:0);
            qToLittleEndian<quint32>((quint32(source&0xfffff)<<12)|quint32(code),bytes.data()+qint64(f)*kFrameBytes+ch*4);}}
    CHECK(write(QDir(path).filePath("part0.bin"),bytes.left(split*kFrameBytes)));
    CHECK(write(QDir(path).filePath("part1.bin"),bytes.mid(split*kFrameBytes)));
    SessionManifestData m;m.metadata.source="deterministic_held_out_pulses";m.metadata.sampleRate=tdm?20000:16000;m.metadata.frameOrigin=origin;
    m.metadata.tdmKnown=true;m.metadata.tdmEnabled=tdm;m.metadata.tdmEvenFirst=false;
    m.parts={{"part0.bin",qint64(split)*kFrameBytes,split},{"part1.bin",qint64(frames-split)*kFrameBytes,frames-split}};
    m.totalFrames=frames;m.totalBytes=qint64(frames)*kFrameBytes;m.complete=true;m.active=false;m.frameValidityKnown=true;
    m.invalidFrameRanges={{3200,24}};m.integrity.upstreamMissingFrames=104;m.stopReason="fixture_complete";
    QString error;CHECK(SessionManifest::write(path,m,&error));return true;
}
static SpikeRuleSet::Snapshot rules(const SpikeDetectConfig &c){
    SpikeElectrodeRules e;auto &x=e.context;x.adcChannel=7;x.electrode=7;x.sourceSampleRate=16000;x.inputGain=c.inputGain;
    x.referenceMode=0;x.filterOrder=c.proc.filterOrder;x.notchHz=c.proc.notchHz;x.highpassHz=c.proc.highpassHz;x.spikeLowpassHz=c.proc.spikeLowpassHz;
    x.absoluteThreshold=c.proc.absoluteThreshold;x.absThresholdV=c.proc.absThresholdV;x.rmsMultiple=c.proc.rmsMultiple;x.negativePolarity=c.proc.negativePolarity;
    x.refractoryMs=c.proc.refractoryMs;x.thresholdOverrideEnabled=true;x.thresholdOverrideV=-100.0*1e-6;x.preSamples=c.preSamples;x.postSamples=c.postSamples;
    e.candidates={{3,{{-.2,.2,-900,-80}}}};return SpikeRuleSet::create(7,{e});
}
static QVector<SpikeArchiveRecord> readEvents(const QString &path){SpikeEventArchiveReader reader;QString error;if(!reader.open(path,&error))return {};
    QVector<SpikeArchiveRecord> all;SpikeArchiveCursor cursor;for(int i=0;i<10000;++i){auto page=reader.readPage({},cursor,64);if(!page.error.isEmpty())return {};all+=page.events;if(page.atEnd)return all;cursor=page.next;}return {};}
static bool runJob(const SpikeOfflineAnalysisRequest &r,SpikeOfflineAnalysisStatus *result){SpikeOfflineAnalysisJob job(r);job.start();CHECK(until([&]{return job.isFinished();},30000));job.wait();*result=job.status();
    if(result->state==SpikeArchiveState::Failed)std::cerr<<"job failure: "<<result->error.toStdString()<<"\n";return true;}
static bool sourceAndBackpressure(const QString &root){
    const auto source=QDir(root).filePath("fixture");CHECK(fixture(source));
    const auto rawHash=hashFile(QDir(source).filePath("part0.bin")),manifestHash=hashFile(QDir(source).filePath("session.json"));
    SpikeOfflineAnalysisRequest request;request.inputPath=QDir(source).filePath("part1.bin");request.config=config();request.rules=rules(request.config);CHECK(request.rules);
    request.thresholdOverridesUvByElectrode={{7,100}};request.displayRetentionPerLane=2;request.queueBlocks=1;request.blockFrames=257;
    request.outputDirectory=QDir(root).filePath("slow");request.archiveOptions.maxQueuedBytes=32768;request.archiveOptions.testWriteDelayMs=25;
    SpikeOfflineAnalysisStatus slow;CHECK(runJob(request,&slow));CHECK(slow.state==SpikeArchiveState::Completed&&slow.archive.finishVerified&&slow.inputEof);
    CHECK(slow.totalFrames==12000&&slow.readFrames==12000&&slow.processedFrames==12000&&slow.sourceSampleRate==16000&&!slow.tdmEnabled&&slow.tdmKnown);
    CHECK(slow.detectedEvents>=20&&slow.archive.writtenEvents==quint64(slow.detectedEvents)&&slow.detectedEvents>=10*slow.retainedEvents);
    CHECK(slow.archive.acceptedEvents==slow.archive.writtenEvents&&slow.archive.peakQueuedBytes<=request.archiveOptions.maxQueuedBytes&&slow.backpressureWaits>0);
    CHECK(slow.sourceIntegrity["missing_source_frames"].toString()=="104"&&slow.sourceIntegrity["invalid_frames"].toString()=="24");
    CHECK(slow.sourceIntegrity["queue_dropped_frames"].toString()=="0"&&slow.sourceIntegrity["unverified_frames"].toString()=="0");
    CHECK(slow.sourceIntegrity["incomplete_analysis_coverage"].toBool()&&slow.sourceIntegrity["incomplete_coverage"].toBool()&&!slow.sourceIntegrity["whole_source_coverage_claimed"].toBool()); // gaps do not change archive lifecycle
    const auto events=readEvents(request.outputDirectory);CHECK(events.size()==slow.detectedEvents);
    for(const auto &r:events){CHECK(r.event.runId==slow.runId&&r.event.eventId>0&&r.event.detectorRevision>0&&r.event.ruleRevision==7);
        CHECK(r.event.classification==SpikeClassificationStatus::Assigned&&r.event.unitId==3&&r.event.sourceSampleRate==16000&&r.event.inputGain==60);
        CHECK(r.event.electrode==7&&r.event.adcChannel==7&&r.event.sampleStride==1&&r.waveform.size()==265);
        CHECK(!(r.event.sourceFrame>=4400&&r.event.sourceFrame<4424));}
    SpikeEventArchiveReader reader;QString error;CHECK(reader.open(request.outputDirectory,&error));QJsonObject metadata;
    CHECK(reader.revisionMetadata(events.first().event.detectorRevision,7,&metadata,&error));
    CHECK(metadata["analysis"].toObject()["source_sample_rate_hz"].toDouble()==16000&&metadata["rule_set"].toObject()["rule_revision"].toString()=="7");
    CHECK(reader.runMetadata()["source"].toObject()["has_manifest"].toBool());
    request.outputDirectory=QDir(root).filePath("fast");request.blockFrames=1024;request.archiveOptions={};SpikeOfflineAnalysisStatus fast;
    CHECK(runJob(request,&fast));CHECK(fast.state==SpikeArchiveState::Completed&&fast.archive.writtenEvents==slow.archive.writtenEvents);
    const auto comparison=readEvents(request.outputDirectory);CHECK(comparison.size()==events.size());
    for(int i=0;i<events.size();++i){CHECK(comparison[i].event.sourceFrame==events[i].event.sourceFrame&&comparison[i].event.electrode==events[i].event.electrode);
        CHECK(comparison[i].waveform.size()==events[i].waveform.size());
        CHECK(std::memcmp(comparison[i].waveform.constData(),events[i].waveform.constData(),size_t(events[i].waveform.size())*sizeof(float))==0);}
    CHECK(hashFile(QDir(source).filePath("part0.bin"))==rawHash&&hashFile(QDir(source).filePath("session.json"))==manifestHash);
    // Source manifest takes precedence over the request's old ADC mode/rate.
    const auto tdm=QDir(root).filePath("tdm_fixture");CHECK(fixture(tdm,true));request.inputPath=tdm;request.outputDirectory=QDir(root).filePath("tdm_archive");
    request.rules.reset();request.config=config();request.config.postSamples=24;request.thresholdOverridesUvByElectrode={{31,100}};
    SpikeOfflineAnalysisStatus t;CHECK(runJob(request,&t));CHECK(t.state==SpikeArchiveState::Completed&&t.tdmKnown&&t.tdmEnabled&&!t.tdmPair02&&t.sourceSampleRate==20000);
    const auto te=readEvents(request.outputDirectory);CHECK(!te.isEmpty());for(const auto &r:te)CHECK(r.event.electrode==31&&r.event.tdmPhase==3&&r.event.sampleStride==4&&r.event.sourceSampleRate==20000);
    return true;
}
static bool cancellationAndFailures(const QString &root){
    SpikeOfflineAnalysisRequest r;r.inputPath=QDir(root).filePath("fixture");r.config=config();r.blockFrames=128;r.queueBlocks=1;r.displayRetentionPerLane=2;
    r.outputDirectory=QDir(root).filePath("cancelled");r.testReadDelayMs=3;
    {SpikeOfflineAnalysisJob job(r);job.start();CHECK(until([&]{return job.status().readFrames>=1024||job.isFinished();}));job.requestCancel();CHECK(until([&]{return job.isFinished();}));job.wait();
        const auto s=job.status();CHECK(s.state==SpikeArchiveState::Cancelled&&s.archive.finishVerified&&!s.inputEof&&s.processedFrames<=s.readFrames&&s.readFrames<s.totalFrames);
        SpikeEventArchiveReader reader;CHECK(reader.open(r.outputDirectory));CHECK(reader.status().state==SpikeArchiveState::Cancelled&&reader.status().finishVerified);
        CHECK(reader.status().writtenEvents==s.archive.writtenEvents);}
    r.testReadDelayMs=0;r.outputDirectory=QDir(root).filePath("read_failed");r.testReadFailureAfterFrames=2048;SpikeOfflineAnalysisStatus failed;
    CHECK(runJob(r,&failed));CHECK(failed.state==SpikeArchiveState::Failed&&!failed.archive.finishVerified&&failed.error.contains("input-read"));
    SpikeEventArchiveReader reader;CHECK(reader.open(r.outputDirectory)&&reader.status().state==SpikeArchiveState::Failed);
    r.testReadFailureAfterFrames=-1;r.outputDirectory=QDir(root).filePath("disk_failed");r.archiveOptions.testFailAfterBytes=22000;
    CHECK(runJob(r,&failed));CHECK(failed.state==SpikeArchiveState::Failed&&!failed.archive.finishVerified&&!failed.error.isEmpty());
    CHECK(reader.open(r.outputDirectory)&&reader.status().recoveredPrefix&&reader.status().writtenEvents==failed.archive.writtenEvents);
    // Once terminal commit starts, Cancel is disabled/no-op while every
    // accepted tail record is flushed. No misleading late-cancel result.
    r.archiveOptions={};r.archiveOptions.testWriteDelayMs=60;r.blockFrames=1024;r.outputDirectory=QDir(root).filePath("final_drain");
    {SpikeOfflineAnalysisJob job(r);job.start();CHECK(until([&]{return job.status().finalizing||job.isFinished();}));
        CHECK(job.status().finalizing&&!job.status().cancellable);job.requestCancel();CHECK(until([&]{return job.isFinished();},15000));job.wait();
        CHECK(job.status().state==SpikeArchiveState::Completed&&job.status().archive.finishVerified&&!job.status().cancellable);}
    // A failed start cannot overwrite the raw recording or an existing archive.
    r.archiveOptions={};r.outputDirectory=r.inputPath;CHECK(runJob(r,&failed));CHECK(failed.state==SpikeArchiveState::Failed&&QFile::exists(QDir(r.inputPath).filePath("session.json")));
    return true;
}
static bool actualDummyHeldOut(const QString &root){
    // Parameters are fixed before observing this fixture. The first 8000 actual
    // Dummy frames are discarded; only the subsequent unseen tail is analyzed.
    auto c=config();c.postSamples=24;c.proc.refractoryMs=1;
    QTcpServer reserve;CHECK(reserve.listen(QHostAddress::LocalHost,0));const int port=reserve.serverPort();reserve.close();
    DummyStreamServer dummy;dummy.setWaveform(DummyWaveform::Spike);QString error;CHECK(dummy.start(port,port,&error));
    QTcpSocket client;client.connectToHost(QHostAddress::LocalHost,port);CHECK(until([&]{return client.state()==QAbstractSocket::ConnectedState;}));
    CHECK(client.write("ctre")==4);client.flush();QByteArray captured;const qint64 required=40000LL*kFrameBytes;
    CHECK(until([&]{captured+=client.readAll();return captured.size()>=required;},12000));client.abort();captured.truncate(required);
    const auto heldout=captured.mid(8000*kFrameBytes);captured.clear();const auto path=QDir(root).filePath("actual_dummy_heldout.bin");CHECK(write(path,heldout));
    SpikeOfflineAnalysisRequest r;r.inputPath=path;r.outputDirectory=QDir(root).filePath("actual_dummy_archive");r.config=c;r.displayRetentionPerLane=1;r.queueBlocks=1;r.blockFrames=511;
    SpikeOfflineAnalysisStatus s;CHECK(runJob(r,&s));CHECK(s.state==SpikeArchiveState::Completed&&s.archive.finishVerified&&s.totalFrames==32000&&s.processedFrames==32000);
    CHECK(s.archive.writtenEvents>1000&&s.detectedEvents>10*s.retainedEvents&&s.archive.writtenEvents==quint64(s.detectedEvents));
    CHECK(!s.tdmKnown&&s.sourceSampleRate==20000&&s.sourceIntegrity["unverified_frames"].toString()=="32000");
    const auto events=readEvents(r.outputDirectory);CHECK(events.size()==qint64(s.archive.writtenEvents));
    for(const auto &e:events)CHECK(e.event.sourceFrame>=0&&e.event.sourceFrame<32000&&e.event.sourceSampleRate==20000&&e.event.inputGain==60&&e.event.runId==s.runId&&e.waveform.size()==33);
    std::cout<<"actual Dummy held-out: "<<s.processedFrames<<" frames, "<<s.archive.writtenEvents<<" archived events, "<<s.retainedEvents<<" retained display events\n";
    return true;
}
int main(int argc,char **argv){QCoreApplication app(argc,argv);QTemporaryDir dir;if(!dir.isValid())return 1;
    if(!sourceAndBackpressure(dir.path())||!cancellationAndFailures(dir.path())||!actualDummyHeldOut(dir.path()))return 2;
    std::cout<<"spike_offline_analysis_smoke OK: multipart/gaps/TDM/provenance, exact bounded lossless backpressure, cancellation/failures and actual Dummy held-out input\n";return 0;}
