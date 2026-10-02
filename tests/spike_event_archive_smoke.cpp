#include "io/spike_event_archive.h"
#include "signal/spike_snippet_store.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace ccv2;
#define CHECK(c) do { if(!(c)){std::cerr<<"FAIL line "<<__LINE__<<": " #c "\n";return false;} } while(false)
static SpikeArchiveRecord event(const QUuid &run,quint64 id,int length=4) {
    SpikeArchiveRecord r;auto &e=r.event;e.runId=run;e.eventId=id;e.detectorRevision=1;e.ruleRevision=0;
    e.epoch=7;e.continuitySegment=id/25;e.sequence=id;e.sourceFrame=qint64(id)*100;e.sourceSampleRate=20000;e.inputGain=60;
    e.lane=0;e.adcChannel=2;e.electrode=2;e.tdmPhase=-1;e.sampleStride=1;e.preSamples=1;e.unitId=id%2?1:-1;
    e.classification=id%2?SpikeClassificationStatus::Assigned:SpikeClassificationStatus::Unassigned;
    for(int i=0;i<length;++i)r.waveform.append(float(qint64(id)*10-i)*0.000001f);return r;
}
static bool same(const SpikeArchiveRecord &a,const SpikeArchiveRecord &b) {
    const auto &x=a.event,&y=b.event;
    return x.runId==y.runId&&x.eventId==y.eventId&&x.detectorRevision==y.detectorRevision&&x.ruleRevision==y.ruleRevision&&
        x.epoch==y.epoch&&x.continuitySegment==y.continuitySegment&&x.sequence==y.sequence&&x.sourceFrame==y.sourceFrame&&
        x.sourceSampleRate==y.sourceSampleRate&&x.inputGain==y.inputGain&&x.lane==y.lane&&x.adcChannel==y.adcChannel&&
        x.electrode==y.electrode&&x.tdmPhase==y.tdmPhase&&x.sampleStride==y.sampleStride&&x.preSamples==y.preSamples&&
        x.unitId==y.unitId&&x.classification==y.classification&&a.waveform.size()==b.waveform.size()&&
        std::memcmp(a.waveform.constData(),b.waveform.constData(),size_t(a.waveform.size())*sizeof(float))==0;
}
static QVector<SpikeArchiveRecord> allEvents(SpikeEventArchiveReader &reader,const SpikeArchiveQuery &query={}) {
    QVector<SpikeArchiveRecord> all;SpikeArchiveCursor cursor;
    for(int i=0;i<10000;++i){auto page=reader.readPage(query,cursor,7,250);if(!page.error.isEmpty())return {};all+=page.events;if(page.atEnd)return all;cursor=page.next;}return {};
}
static bool copyArchive(const QString &source,const QString &destination) {
    if(!QDir().mkpath(destination))return false;
    for(const auto &name:QDir(source).entryList(QDir::Files))if(!QFile::copy(QDir(source).filePath(name),QDir(destination).filePath(name)))return false;return true;
}
static QVector<qint64> eventOffsets(const QString &path) {
    QFile f(path);QVector<qint64> offsets;if(!f.open(QIODevice::ReadOnly))return offsets;f.seek(32);
    while(f.pos()<f.size()){qint64 offset=f.pos();auto h=f.read(16);if(h.size()!=16)break;auto n=qFromLittleEndian<quint32>(h.constData());
        if(qFromLittleEndian<quint16>(h.constData()+4)==3)offsets.append(offset);if(!f.seek(offset+48+n))break;}return offsets;
}
static bool fullRunAndRecovery(const QString &root) {
    const auto run=QUuid::createUuid();const auto path=QDir(root).filePath("full");QString error;
    SpikeEventArchiveWriter writer;SpikeArchiveOptions options;options.maxQueuedBytes=4096;options.testWriteDelayMs=2;
    CHECK(writer.begin(path,run,{{"source","deterministic_fixture"}},options,&error));
    const QJsonObject rev1{{"gain",60},{"threshold_uv",75},{"rules",QJsonObject{{"revision","0"}}}};
    const QJsonObject rev2{{"gain",180},{"threshold_uv",80},{"rules",QJsonObject{{"revision","1"},{"unit",2}}}};
    CHECK(writer.enqueueRevision(1,0,rev1)==SpikeArchiveEnqueueResult::Accepted);
    CHECK(writer.enqueueRevision(1,0,rev1)==SpikeArchiveEnqueueResult::Accepted);
    SpikeSnippetStore display;CHECK(display.configure(1,4,8));display.resetTimeline(7);
    QVector<SpikeArchiveRecord> expected;
    for(int batch=0;batch<12;++batch){
        if(batch==6)CHECK(writer.enqueueRevision(2,1,rev2)==SpikeArchiveEnqueueResult::Accepted);
        QVector<SpikeArchiveRecord> records;
        for(int i=0;i<8;++i){auto r=event(run,quint64(batch*8+i+1));
            if(batch>=6){r.event.detectorRevision=2;r.event.ruleRevision=1;r.event.inputGain=180;}
            // A pending crossing retains its actual old revision after a config change.
            if(batch==6&&i==0){r.event.detectorRevision=1;r.event.ruleRevision=0;r.event.inputGain=60;}
            CHECK(display.addEventIfEpoch(0,r.waveform.constData(),r.event));records.append(r);expected.append(r);
        }
        CHECK(writer.enqueueBatch(records,SpikeArchiveEnqueueMode::OfflineBlocking)==SpikeArchiveEnqueueResult::Accepted);
        // Source buffers may be reused immediately; queued archive bytes own a copy.
        records[0].waveform.fill(999.0f);
    }
    CHECK(display.snapshotLane(0).events.size()==8);CHECK(expected.size()>=10*display.capacity());
    SpikeArchiveAnnotation active;active.runId=run;active.eventId=1;active.unitId=4;active.note="historical event outside display retention";
    CHECK(writer.enqueueManualLabels({active})==SpikeArchiveEnqueueResult::Accepted);
    CHECK(writer.finish(SpikeArchiveState::Completed,{{"source_gaps",3},{"unverified_source",true}}));writer.wait();
    auto status=writer.status();CHECK(status.writerFinished);CHECK(status.state==SpikeArchiveState::Completed&&status.finishVerified&&status.writtenEvents==96&&status.acceptedEvents==96);
    CHECK(status.peakQueuedBytes<=options.maxQueuedBytes);
    SpikeEventArchiveReader reader;CHECK(reader.open(path,&error));CHECK(reader.status().finishVerified&&reader.status().writtenEvents==96);
    CHECK(reader.status().sourceIntegrity["source_gaps"].toInt()==3);CHECK(reader.runMetadata()["source"].toString()=="deterministic_fixture");
    auto actual=allEvents(reader);CHECK(actual.size()==expected.size());for(int i=0;i<actual.size();++i)CHECK(same(actual[i],expected[i]));
    QJsonObject metadata;CHECK(reader.revisionMetadata(1,0,&metadata,&error)&&metadata==rev1);CHECK(reader.revisionMetadata(2,1,&metadata,&error)&&metadata==rev2);
    SpikeArchiveQuery query;query.electrode=2;query.filterEpoch=true;query.epoch=7;query.sourceTimeFrom=.1;query.sourceTimeTo=.2;
    auto selected=allEvents(reader,query);CHECK(selected.size()==21&&selected.first().event.eventId==20&&selected.last().event.eventId==40);
    query.electrode=999;CHECK(allEvents(reader,query).isEmpty());
    active.unitId=7;active.note="revised manual candidate";
    SpikeArchiveAnnotation second=active;second.eventId=96;second.unitId=-1;
    CHECK(SpikeEventArchiveReader::appendManualLabels(path,{active,second},&error));CHECK(reader.open(path,&error));
    QVector<SpikeArchiveAnnotation> labels;qint64 cursor=32;
    for(int i=0;i<100;++i){auto page=reader.readAnnotations(1,cursor,1);CHECK(page.error.isEmpty());labels+=page.annotations;if(page.atEnd)break;cursor=page.nextOffset;}
    CHECK(labels.size()==2&&labels[0].unitId==4&&labels[1].unitId==7&&labels[0].annotationId<labels[1].annotationId);
    CHECK(allEvents(reader).first().event.unitId==1); // immutable original automatic assignment
    CHECK(!SpikeEventArchiveReader::appendManualLabel(path,run,999,2,{},&error));
    CHECK(!SpikeEventArchiveReader::appendManualLabel(path,QUuid::createUuid(),1,2,{},&error));
    SpikeEventArchiveWriter collision;CHECK(!collision.begin(path,run,{}, {},&error));
    const auto offsets=eventOffsets(QDir(path).filePath("events.nlsa"));CHECK(offsets.size()==12);
    for(int mode=0;mode<4;++mode){const auto damaged=QDir(root).filePath(QString("damaged%1").arg(mode));CHECK(copyArchive(path,damaged));
        QFile f(QDir(damaged).filePath("events.nlsa"));CHECK(f.open(QIODevice::ReadWrite));
        if(mode==0)CHECK(f.resize(offsets.last()+40));
        if(mode==1){CHECK(f.seek(offsets.last()+20));auto b=f.read(1);CHECK(b.size()==1);b[0]=char(b[0]^0x40);CHECK(f.seek(offsets.last()+20)&&f.write(b)==1);}
        if(mode==2){CHECK(f.seek(offsets.last()));CHECK(f.write(QByteArray(4,char(0xff)))==4);}
        f.close();if(mode==3)CHECK(QFile::remove(QDir(damaged).filePath("finish.json")));
        SpikeEventArchiveReader recovered;CHECK(recovered.open(damaged,&error));CHECK(recovered.status().recoveredPrefix&&!recovered.status().finishVerified);
        CHECK(recovered.status().writtenEvents==(mode==3?96:88));CHECK(allEvents(recovered).size()==(mode==3?96:88));
    }
    // Annotation tail recovery preserves earlier history, and refuses appends to
    // a damaged tail instead of hiding or rewriting it.
    const auto damagedAnnotations=QDir(root).filePath("damaged_annotations");CHECK(copyArchive(path,damagedAnnotations));
    QFile annotations(QDir(damagedAnnotations).filePath("annotations.nlsa"));CHECK(annotations.open(QIODevice::ReadWrite));CHECK(annotations.resize(annotations.size()-9));annotations.close();
    SpikeEventArchiveReader ar;CHECK(ar.open(damagedAnnotations,&error));auto ap=ar.readAnnotations();CHECK(ap.recoveredPrefix&&ap.annotations.size()==2&&!ap.error.isEmpty());
    CHECK(!SpikeEventArchiveReader::appendManualLabel(damagedAnnotations,run,1,8,{},&error));
    // Checksums alone do not make a history valid: reordered, duplicated, or
    // semantically forged intact records stop the annotation prefix as well.
    for(int mode=0;mode<4;++mode){const auto damaged=QDir(root).filePath(QString("annotation_semantics%1").arg(mode));CHECK(copyArchive(path,damaged));
        QFile f(QDir(damaged).filePath("annotations.nlsa"));CHECK(f.open(QIODevice::ReadWrite));auto bytes=f.readAll();
        const int firstLength=int(qFromLittleEndian<quint32>(bytes.constData()+32))+48;
        const auto first=bytes.mid(32,firstLength),second=bytes.mid(32+firstLength);
        if(mode==0)bytes=bytes.left(32)+second+first;
        if(mode==1)bytes+=first;
        if(mode==2){qToLittleEndian<quint64>(999,bytes.data()+32+16+16);
            const auto digest=QCryptographicHash::hash(bytes.mid(32,firstLength-32),QCryptographicHash::Sha256);bytes.replace(32+firstLength-32,32,digest);}
        if(mode==3){const int position=32+firstLength;const int length=second.size();qToLittleEndian<quint64>(2,bytes.data()+position+16+28);
            const auto digest=QCryptographicHash::hash(bytes.mid(position,length-32),QCryptographicHash::Sha256);bytes.replace(position+length-32,32,digest);}
        CHECK(f.resize(0)&&f.seek(0)&&f.write(bytes)==bytes.size());f.close();SpikeEventArchiveReader recovered;CHECK(recovered.open(damaged,&error));
        const auto page=recovered.readAnnotations();CHECK(recovered.status().finishVerified&&page.recoveredPrefix&&!page.error.isEmpty());
        CHECK(page.annotations.size()==(mode==1?3:(mode==3?2:1)));
        CHECK(!SpikeEventArchiveReader::appendManualLabel(damaged,run,1,9,{},&error));
    }
    std::atomic_bool cancelled{true};SpikeEventArchiveReader cancelledReader;
    CHECK(!cancelledReader.open(path,&error,&cancelled)&&error.contains("cancelled"));
    QFile intact(QDir(path).filePath("annotations.nlsa"));CHECK(intact.open(QIODevice::ReadOnly));const auto before=intact.readAll();intact.close();
    CHECK(!SpikeEventArchiveReader::appendManualLabel(path,run,1,9,{},&error,&cancelled));
    CHECK(intact.open(QIODevice::ReadOnly)&&intact.readAll()==before);
    return true;
}
static bool pressureAndFailure(const QString &root) {
    QString error;
    // Live producers never wait for disk space and queue exhaustion is terminal,
    // with all already accepted records drained and the failure visible on reopen.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;SpikeArchiveOptions o;o.maxQueuedBytes=512;o.testWriteDelayMs=60;
        auto path=QDir(root).filePath("live");CHECK(w.begin(path,run,{},o,&error));CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);
        int accepted=0;for(int i=1;i<20;++i){auto r=w.enqueueBatch({event(run,i)},SpikeArchiveEnqueueMode::LiveNonBlocking);if(r==SpikeArchiveEnqueueResult::Failed)break;CHECK(r==SpikeArchiveEnqueueResult::Accepted);++accepted;}
        CHECK(accepted>0&&accepted<19);CHECK(!w.status().writerFinished);w.wait();CHECK(w.status().writerFinished);CHECK(w.status().state==SpikeArchiveState::Failed&&w.status().writtenEvents==quint64(accepted)&&w.status().queuedBytes==0);
        SpikeEventArchiveReader r;CHECK(r.open(path,&error)&&r.status().state==SpikeArchiveState::Failed&&r.status().writtenEvents==quint64(accepted));}
    // A blocked offline producer can cancel promptly; its rejected batch never
    // appears, while previously accepted work is drained and marked cancelled.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;SpikeArchiveOptions o;o.maxQueuedBytes=256;o.testWriteDelayMs=100;
        auto path=QDir(root).filePath("cancel");CHECK(w.begin(path,run,{},o,&error));CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);
        CHECK(w.enqueueBatch({event(run,1)},SpikeArchiveEnqueueMode::OfflineBlocking)==SpikeArchiveEnqueueResult::Accepted);
        std::atomic_bool cancel{false};std::thread t([&]{std::this_thread::sleep_for(std::chrono::milliseconds(15));cancel=true;});
        QElapsedTimer timer;timer.start();auto result=w.enqueueBatch({event(run,2)},SpikeArchiveEnqueueMode::OfflineBlocking,&cancel);t.join();
        CHECK(result==SpikeArchiveEnqueueResult::Cancelled&&timer.elapsed()<500);CHECK(w.finish(SpikeArchiveState::Cancelled,{{"stopped_early",true}}));w.wait();
        SpikeEventArchiveReader r;CHECK(r.open(path,&error)&&r.status().finishVerified&&r.status().state==SpikeArchiveState::Cancelled&&r.status().writtenEvents==1);}
    // Deterministic partial disk write, recovered exactly to last complete frame.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;SpikeArchiveOptions o;o.maxQueuedBytes=1024;o.testFailAfterBytes=900;
        auto path=QDir(root).filePath("disk_failure");CHECK(w.begin(path,run,{},o,&error));CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);
        for(int i=1;i<=20;++i)if(w.enqueueBatch({event(run,i)},SpikeArchiveEnqueueMode::OfflineBlocking)!=SpikeArchiveEnqueueResult::Accepted)break;
        w.finish(SpikeArchiveState::Drained);w.wait();CHECK(w.status().state==SpikeArchiveState::Failed&&w.status().writtenEvents<w.status().acceptedEvents);
        SpikeEventArchiveReader r;CHECK(r.open(path,&error)&&r.status().recoveredPrefix&&r.status().state==SpikeArchiveState::Failed);
        CHECK(r.status().writtenEvents==w.status().writtenEvents&&allEvents(r).size()==qint64(w.status().writtenEvents));}
    // A source/read/processing failure is distinct from user cancellation and
    // still drains every already accepted immutable event.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;auto path=QDir(root).filePath("processing_failure");
        CHECK(w.begin(path,run,{}, {},&error));CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);
        CHECK(w.enqueueBatch({event(run,1),event(run,2)},SpikeArchiveEnqueueMode::OfflineBlocking)==SpikeArchiveEnqueueResult::Accepted);
        CHECK(w.finish(SpikeArchiveState::Failed,{{"error","fixture read failed"},{"invalid_frames",5}}));w.wait();
        CHECK(w.status().state==SpikeArchiveState::Failed&&w.status().writtenEvents==2&&!w.status().finishVerified);
        SpikeEventArchiveReader r;CHECK(r.open(path,&error)&&r.status().state==SpikeArchiveState::Failed&&r.status().writtenEvents==2&&!r.status().finishVerified);
        CHECK(r.status().error=="fixture read failed"&&r.status().sourceIntegrity["invalid_frames"].toInt()==5);}
    // Empty runs are qualified by an explicit terminal marker, and source
    // integrity remains independent of successful event storage.
    for(auto state:{SpikeArchiveState::Completed,SpikeArchiveState::Drained}){const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;
        auto path=QDir(root).filePath(spikeArchiveStateName(state));CHECK(w.begin(path,run,{}, {},&error));CHECK(w.finish(state,{{"invalid_frames",12}}));w.wait();
        SpikeEventArchiveReader r;CHECK(r.open(path,&error)&&r.status().finishVerified&&r.status().state==state&&r.status().writtenEvents==0&&r.status().sourceIntegrity["invalid_frames"].toInt()==12);}
    // Terminal envelope limits include the final manifest/marker fields, not
    // just the nested integrity object; rejection leaves the run finishable.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;auto path=QDir(root).filePath("terminal_limits");
        CHECK(w.begin(path,run,{}, {},&error));
        CHECK(!w.finish(SpikeArchiveState::Completed,{{"detail",QString(8*1024*1024-100,'x')}}));
        CHECK(w.finish(SpikeArchiveState::Cancelled));w.wait();SpikeEventArchiveReader r;CHECK(r.open(path,&error)&&r.status().finishVerified);}
    // Variable-length TDM waveforms, disjoint identities above JSON's exact
    // integer range, alternate epochs and classification states round-trip.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;auto path=QDir(root).filePath("variable_tdm");
        CHECK(w.begin(path,run,{}, {},&error));CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);
        auto a=event(run,9007199254740993ULL,5),b=event(run,9007199254741003ULL,19);
        a.event.tdmPhase=3;a.event.electrode=11;a.event.sampleStride=4;a.event.preSamples=2;a.event.unitId=-1;a.event.classification=SpikeClassificationStatus::Incompatible;
        a.waveform[0]=-0.0f;b.event.epoch=8;b.event.classification=SpikeClassificationStatus::Ambiguous;b.event.unitId=-1;
        CHECK(w.enqueueBatch({a,b},SpikeArchiveEnqueueMode::OfflineBlocking)==SpikeArchiveEnqueueResult::Accepted);
        CHECK(w.finish(SpikeArchiveState::Completed));w.wait();SpikeEventArchiveReader r;CHECK(r.open(path,&error));auto events=allEvents(r);
        CHECK(events.size()==2&&same(events[0],a)&&same(events[1],b));
        CHECK(SpikeEventArchiveReader::appendManualLabel(path,run,a.event.eventId,5,{},&error));
        CHECK(!SpikeEventArchiveReader::appendManualLabel(path,run,a.event.eventId+1,5,{},&error));}
    // Reused IDs and mutated metadata cannot silently replace historical facts.
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;auto path=QDir(root).filePath("identity");CHECK(w.begin(path,run,{}, {},&error));
        CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);CHECK(w.enqueueBatch({event(run,1)},SpikeArchiveEnqueueMode::OfflineBlocking)==SpikeArchiveEnqueueResult::Accepted);
        CHECK(w.enqueueBatch({event(run,1)},SpikeArchiveEnqueueMode::OfflineBlocking)==SpikeArchiveEnqueueResult::Failed);w.wait();CHECK(w.status().state==SpikeArchiveState::Failed);}
    {const auto run=QUuid::createUuid();SpikeEventArchiveWriter w;auto path=QDir(root).filePath("revision");CHECK(w.begin(path,run,{}, {},&error));
        CHECK(w.enqueueRevision(1,0,{})==SpikeArchiveEnqueueResult::Accepted);CHECK(w.enqueueRevision(1,0,{{"changed",true}})==SpikeArchiveEnqueueResult::Failed);w.wait();CHECK(w.status().state==SpikeArchiveState::Failed);}
    return true;
}
int main(int argc,char **argv){QCoreApplication app(argc,argv);QTemporaryDir dir;if(!dir.isValid())return 1;
    if(!fullRunAndRecovery(dir.path())||!pressureAndFailure(dir.path()))return 2;
    std::cout<<"spike_event_archive_smoke OK: 12x display retention, exact reopen, revisions, immutable labels, bounded backpressure, cancellation, live failure and verified-prefix recovery\n";return 0;}
