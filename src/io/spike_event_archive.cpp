#include "spike_event_archive.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace ccv2 {
namespace {
constexpr qint64 HeaderBytes = 32;
constexpr quint32 MaxPayload = 16 * 1024 * 1024;
constexpr int MaxJson = 8 * 1024 * 1024;
constexpr int MaxWave = 16384;
constexpr int MaxBatch = 4096;
constexpr int MaxPage = 256;
constexpr int MaxRegistrations = 65536;
using RevisionKey = QPair<quint64,quint64>;
using EventRanges = QMap<quint64,quint64>;
bool containsEvent(const EventRanges &ranges,quint64 id) {auto it=ranges.upperBound(id);if(it==ranges.begin())return false;--it;return id<=it.value();}
bool addEventRange(EventRanges &ranges,quint64 first,quint64 last) {if(!ranges.isEmpty()&&ranges.last()!=std::numeric_limits<quint64>::max()&&ranges.last()+1==first){ranges.last()=last;return true;}if(ranges.size()>=MaxRegistrations)return false;ranges.insert(first,last);return true;}
constexpr quint16 RunRecord = 1, RevisionRecord = 2, EventsRecord = 3, TerminalRecord = 4, AnnotationRecord = 5;
const char Magic[8] = {'N','L','S','P','K','A','R','\0'};
QString eventsPath(const QString &dir) { return QDir(dir).filePath(QStringLiteral("events.nlsa")); }
QString annotationsPath(const QString &dir) { return QDir(dir).filePath(QStringLiteral("annotations.nlsa")); }
void setError(QString *out, const QString &error) { if (out) *out = error; }

struct Encoder {
    QByteArray b;
    void u8(quint8 v) { b.append(char(v)); }
    void u16(quint16 v) { char s[2]; qToLittleEndian(v, s); b.append(s, 2); }
    void u32(quint32 v) { char s[4]; qToLittleEndian(v, s); b.append(s, 4); }
    void u64(quint64 v) { char s[8]; qToLittleEndian(v, s); b.append(s, 8); }
    void i32(qint32 v) { u32(quint32(v)); }
    void i64(qint64 v) { u64(quint64(v)); }
    void f32(float v) { quint32 n; static_assert(sizeof(n)==sizeof(v), "float32 required"); std::memcpy(&n,&v,4); u32(n); }
    void f64(double v) { quint64 n; static_assert(sizeof(n)==sizeof(v), "float64 required"); std::memcpy(&n,&v,8); u64(n); }
    void bytes(const QByteArray &v) { u32(quint32(v.size())); b += v; }
    void json(const QJsonObject &v) { bytes(QJsonDocument(v).toJson(QJsonDocument::Compact)); }
};
struct Decoder {
    const QByteArray &b;
    qsizetype p = 0;
    bool ok = true;
    bool need(qsizetype n) { if (n < 0 || p > b.size() || n > b.size()-p) {ok=false;return false;} return ok; }
    quint8 u8() { if(!need(1))return 0; return quint8(b[p++]); }
    quint16 u16() { if(!need(2))return 0; auto v=qFromLittleEndian<quint16>(b.constData()+p);p+=2;return v; }
    quint32 u32() { if(!need(4))return 0; auto v=qFromLittleEndian<quint32>(b.constData()+p);p+=4;return v; }
    quint64 u64() { if(!need(8))return 0; auto v=qFromLittleEndian<quint64>(b.constData()+p);p+=8;return v; }
    qint32 i32() { auto n=u32();qint32 v;std::memcpy(&v,&n,4);return v; }
    qint64 i64() { auto n=u64();qint64 v;std::memcpy(&v,&n,8);return v; }
    float f32() { auto n=u32();float v;std::memcpy(&v,&n,4);return v; }
    double f64() { auto n=u64();double v;std::memcpy(&v,&n,8);return v; }
    QByteArray bytes(int max) { auto n=u32();if(n>quint32(max)||!need(n)){ok=false;return {};}auto v=b.mid(p,n);p+=n;return v; }
    QJsonObject json() { auto data=bytes(MaxJson);QJsonParseError e;auto doc=QJsonDocument::fromJson(data,&e);if(e.error!=QJsonParseError::NoError||!doc.isObject())ok=false;return doc.object(); }
    bool done() const { return ok && p==b.size(); }
};
QByteArray header(const QUuid &run) {
    Encoder e;e.b.append(Magic,8);e.u16(1);e.u16(0);e.u32(HeaderBytes);e.b+=run.toRfc4122();return e.b;
}
bool readHeader(QFile &f, QUuid *run, QString *error) {
    const auto b=f.read(HeaderBytes);if(b.size()!=HeaderBytes||std::memcmp(b.constData(),Magic,8)!=0){setError(error,"Invalid or truncated spike archive header");return false;}
    Decoder d{b};d.p=8;auto major=d.u16();auto minor=d.u16();auto bytes=d.u32();
    if(major!=1||minor!=0||bytes!=HeaderBytes){setError(error,"Unsupported spike archive version");return false;}
    *run=QUuid::fromRfc4122(b.mid(16,16));if(run->isNull()){setError(error,"Null archive run identity");return false;}return true;
}
QByteArray frame(quint16 kind, quint64 ordinal, const QByteArray &payload) {
    Encoder e;e.u32(quint32(payload.size()));e.u16(kind);e.u16(1);e.u64(ordinal);e.b+=payload;e.b+=QCryptographicHash::hash(e.b,QCryptographicHash::Sha256);return e.b;
}
struct Frame { quint16 kind=0;quint64 ordinal=0;qint64 start=0,end=0;QByteArray payload,digest; };
bool readFrame(QFile &f, qint64 bound, Frame *out, QString *error,const std::atomic_bool *cancel=nullptr) {
    if(cancel&&cancel->load()){setError(error,"Archive read cancelled");return false;}
    out->start=f.pos();
    if(bound-out->start<48){setError(error,"Truncated record header or checksum");return false;}
    auto h=f.read(16);if(h.size()!=16){setError(error,f.errorString());return false;}
    Decoder d{h};auto size=d.u32();out->kind=d.u16();auto schema=d.u16();out->ordinal=d.u64();
    if(size>MaxPayload||schema!=1||size>quint64(bound-f.pos()-32)){setError(error,"Invalid record length, version, or truncated payload");return false;}
    out->payload.clear();out->payload.reserve(size);QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(h);
    while(out->payload.size()<size){
        if(cancel&&cancel->load()){setError(error,"Archive read cancelled");return false;}
        const auto bytes=f.read(qMin<qint64>(65536,size-out->payload.size()));
        if(bytes.isEmpty()){setError(error,"Truncated spike archive payload");return false;}
        hash.addData(bytes);out->payload+=bytes;
    }
    out->digest=f.read(32);
    if(cancel&&cancel->load()){setError(error,"Archive read cancelled");return false;}
    if(out->payload.size()!=size||out->digest.size()!=32||hash.result()!=out->digest){setError(error,"Spike archive record checksum mismatch");return false;}
    out->end=f.pos();return true;
}
bool jsonWithinLimit(const QJsonObject &obj) {
    const auto bytes=QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if(bytes.size()>MaxJson)return false;
    QJsonParseError error;const auto parsed=QJsonDocument::fromJson(bytes,&error);
    return error.error==QJsonParseError::NoError&&parsed.isObject();
}
bool atomicJson(const QString &path,const QJsonObject &obj,QString *error) {
    QSaveFile f(path);f.setDirectWriteFallback(false);const auto b=QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if(!f.open(QIODevice::WriteOnly)||f.write(b)!=b.size()||!f.commit()){setError(error,f.errorString());return false;}return true;
}
QJsonObject readJson(const QString &path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>MaxJson)return {};return QJsonDocument::fromJson(f.readAll()).object();
}
bool validEvent(const SpikeArchiveRecord &r, const QUuid &run, QString *error) {
    const auto &e=r.event;
    if(e.runId!=run||e.eventId==0||e.lane<0||e.adcChannel<0||e.electrode<0||e.sourceFrame<0||
       !std::isfinite(e.sourceSampleRate)||e.sourceSampleRate<=0||!std::isfinite(e.inputGain)||e.inputGain<=0||
       e.tdmPhase < -1||e.tdmPhase>3||e.sampleStride<1||e.sampleStride>1000000||e.unitId < -1||
       quint8(e.classification)>quint8(SpikeClassificationStatus::Manual)||r.waveform.isEmpty()||r.waveform.size()>MaxWave||
       e.preSamples<0||e.preSamples>=r.waveform.size()) {setError(error,"Invalid event identity, provenance, or waveform dimensions");return false;}
    for(float v:r.waveform)if(!std::isfinite(v)){setError(error,"Nonfinite waveform sample");return false;}
    return true;
}
void encodeEvent(Encoder &d,const SpikeArchiveRecord &r) {
    const auto &e=r.event;d.b+=e.runId.toRfc4122();d.u64(e.eventId);d.u64(e.detectorRevision);d.u64(e.ruleRevision);
    d.u64(e.epoch);d.u64(e.continuitySegment);d.u64(e.sequence);d.i64(e.sourceFrame);d.f64(e.sourceSampleRate);d.f64(e.inputGain);
    d.i32(e.lane);d.i32(e.adcChannel);d.i32(e.electrode);d.i32(e.tdmPhase);d.i32(e.sampleStride);d.i32(e.preSamples);d.i32(e.unitId);d.u8(quint8(e.classification));
    d.u32(quint32(r.waveform.size()));for(float v:r.waveform)d.f32(v);
}
bool decodeEvents(const QByteArray &b,const QUuid &run,QVector<SpikeArchiveRecord> *records,QString *error) {
    Decoder d{b};auto count=d.u32();if(count==0||count>MaxBatch){setError(error,"Invalid event batch count");return false;}
    records->clear();records->reserve(int(count));
    for(quint32 i=0;i<count&&d.ok;++i){
        SpikeArchiveRecord r;auto &e=r.event;if(!d.need(16))break;e.runId=QUuid::fromRfc4122(b.mid(d.p,16));d.p+=16;
        e.eventId=d.u64();e.detectorRevision=d.u64();e.ruleRevision=d.u64();e.epoch=d.u64();e.continuitySegment=d.u64();e.sequence=d.u64();
        e.sourceFrame=d.i64();e.sourceSampleRate=d.f64();e.inputGain=d.f64();e.lane=d.i32();e.adcChannel=d.i32();e.electrode=d.i32();e.tdmPhase=d.i32();
        e.sampleStride=d.i32();e.preSamples=d.i32();e.unitId=d.i32();e.classification=SpikeClassificationStatus(d.u8());auto n=d.u32();
        if(n==0||n>MaxWave||!d.need(qsizetype(n)*4)){d.ok=false;break;}
        r.waveform.reserve(int(n));for(quint32 j=0;j<n;++j)r.waveform.append(d.f32());
        if(!validEvent(r,run,error))return false;records->append(std::move(r));
    }
    if(!d.done()){setError(error,"Malformed event batch payload");return false;}return true;
}
SpikeArchiveState parseState(const QString &s) {
    if(s=="completed")return SpikeArchiveState::Completed;if(s=="drained")return SpikeArchiveState::Drained;if(s=="cancelled")return SpikeArchiveState::Cancelled;
    if(s=="failed")return SpikeArchiveState::Failed;return SpikeArchiveState::Running;
}
bool terminalState(SpikeArchiveState s) {return s==SpikeArchiveState::Completed||s==SpikeArchiveState::Drained||s==SpikeArchiveState::Cancelled;}
QJsonObject statusJson(const SpikeArchiveStatus &s) {
    return {{"format","nl-spike-event-archive"},{"schema_version",1},{"run_id",s.runId.toString(QUuid::WithoutBraces)},
        {"archive_state",spikeArchiveStateName(s.state)},{"accepted_events",QString::number(s.acceptedEvents)},
        {"written_events",QString::number(s.writtenEvents)},{"last_event_id",QString::number(s.lastEventId)},
        {"source_integrity",s.sourceIntegrity},{"error",s.error}};
}
bool matches(const SpikeEvent &e,const SpikeArchiveQuery &q) {
    return (q.electrode<0||q.electrode==e.electrode)&&(!q.filterEpoch||q.epoch==e.epoch)&&
        (q.sourceTimeFrom<0||e.sourceTimeSeconds()>=q.sourceTimeFrom)&&(q.sourceTimeTo<0||e.sourceTimeSeconds()<=q.sourceTimeTo);
}
}

QString spikeArchiveStateName(SpikeArchiveState state) {
    switch(state){case SpikeArchiveState::Idle:return "idle";case SpikeArchiveState::Running:return "running";
    case SpikeArchiveState::Completed:return "completed";case SpikeArchiveState::Drained:return "drained";
    case SpikeArchiveState::Cancelled:return "cancelled";case SpikeArchiveState::Failed:return "failed";}return "failed";
}

struct SpikeEventArchiveWriter::Impl {
    mutable std::mutex mutex;
    std::condition_variable ready,space;
    std::thread worker;
    struct Pending {QByteArray data;quint64 events=0,lastId=0;};
    std::deque<Pending> queue;
    SpikeArchiveStatus status;
    SpikeArchiveOptions options;
    QString dir;
    quint64 ordinal=1, annotationId=0;
    QHash<RevisionKey,QByteArray> revisions;
    EventRanges eventRanges;
    bool finishing=false;
    SpikeArchiveState requestedTerminal=SpikeArchiveState::Cancelled;
    void failLocked(const QString &e) { status.state=SpikeArchiveState::Failed;if(status.error.isEmpty())status.error=e;finishing=true;space.notify_all();ready.notify_all(); }
    SpikeArchiveEnqueueResult reserve(std::unique_lock<std::mutex> &lock,qint64 bytes,SpikeArchiveEnqueueMode mode,const std::atomic_bool *cancel) {
        if(bytes>options.maxQueuedBytes){failLocked("Archive record exceeds bounded writer queue capacity");return SpikeArchiveEnqueueResult::Failed;}
        while(status.state==SpikeArchiveState::Running&&!finishing&&status.queuedBytes>options.maxQueuedBytes-bytes){
            if(cancel&&cancel->load())return SpikeArchiveEnqueueResult::Cancelled;
            if(mode==SpikeArchiveEnqueueMode::LiveNonBlocking){failLocked("Live archive writer queue exhausted; acquisition archive is incomplete");return SpikeArchiveEnqueueResult::Failed;}
            space.wait_for(lock,std::chrono::milliseconds(10));
        }
        if(cancel&&cancel->load())return SpikeArchiveEnqueueResult::Cancelled;
        if(status.state==SpikeArchiveState::Failed)return SpikeArchiveEnqueueResult::Failed;
        if(status.state!=SpikeArchiveState::Running||finishing)return SpikeArchiveEnqueueResult::NotRunning;
        return SpikeArchiveEnqueueResult::Accepted;
    }
    void push(QByteArray data,quint64 count=0,quint64 last=0) {
        status.queuedBytes+=data.size();status.peakQueuedBytes=std::max(status.peakQueuedBytes,status.queuedBytes);
        status.acceptedEvents+=count;if(count)status.lastEventId=last;
        queue.push_back({std::move(data),count,last});ready.notify_one();
    }
    void run() {
        QFile file(eventsPath(dir));QString error;
        if(!file.open(QIODevice::WriteOnly|QIODevice::Append)){std::lock_guard<std::mutex> l(mutex);failLocked(file.errorString());}
        bool ioFailed=!file.isOpen();
        while(!ioFailed){
            Pending p;
            {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return !queue.empty()||finishing;});
                if(queue.empty())break;p=std::move(queue.front());queue.pop_front();}
            if(options.testWriteDelayMs>0)std::this_thread::sleep_for(std::chrono::milliseconds(options.testWriteDelayMs));
            qint64 n=-1;
            if(options.testFailAfterBytes>=0&&file.pos()+p.data.size()>options.testFailAfterBytes){
                qint64 partial=std::max<qint64>(0,options.testFailAfterBytes-file.pos());if(partial)file.write(p.data.constData(),partial);file.flush();error="Injected archive disk-write failure";
            }else {n=file.write(p.data);if(n==p.data.size()&&!file.flush()){n=-1;error=file.errorString();}}
            {std::lock_guard<std::mutex> lock(mutex);status.queuedBytes-=p.data.size();space.notify_all();
                if(n!=p.data.size()){failLocked(error.isEmpty()?file.errorString():error);ioFailed=true;}
                else {status.writtenEvents+=p.events;status.verifiedBytes=file.pos();}}
        }
        SpikeArchiveStatus finalStatus;quint64 terminalOrdinal=0;
        {std::lock_guard<std::mutex> lock(mutex);
            if(ioFailed){queue.clear();status.queuedBytes=0;space.notify_all();}
            finalStatus=status;
            if(finalStatus.state!=SpikeArchiveState::Failed)finalStatus.state=requestedTerminal;
            terminalOrdinal=++ordinal;}
        QByteArray finishDigest;
        if(!ioFailed&&terminalState(finalStatus.state)){
            Encoder e;e.json(statusJson(finalStatus));auto data=frame(TerminalRecord,terminalOrdinal,e.b);
            if((options.testFailAfterBytes>=0&&file.pos()+data.size()>options.testFailAfterBytes)||file.write(data)!=data.size()||!file.flush()){
                finalStatus.state=SpikeArchiveState::Failed;finalStatus.error="Failed to commit archive terminal record";
            }else{finishDigest=data.right(32);finalStatus.verifiedBytes=file.pos();}
        }
        file.close();
        auto manifest=statusJson(finalStatus);manifest["verified_bytes"]=QString::number(finalStatus.verifiedBytes);
        if(!atomicJson(QDir(dir).filePath("archive.json"),manifest,&error)){
            finalStatus.state=SpikeArchiveState::Failed;finalStatus.error="Failed to commit archive manifest: "+error;
        }
        if(terminalState(finalStatus.state)&&!finishDigest.isEmpty()){
            auto finish=manifest;finish["terminal_sha256"]=QString::fromLatin1(finishDigest.toHex());
            if(!atomicJson(QDir(dir).filePath("finish.json"),finish,&error)){
                finalStatus.state=SpikeArchiveState::Failed;finalStatus.error="Failed to commit archive finish marker: "+error;
                atomicJson(QDir(dir).filePath("archive.json"),statusJson(finalStatus),nullptr);
            }else finalStatus.finishVerified=true;
        }
        {std::lock_guard<std::mutex> lock(mutex);status=finalStatus;status.writerFinished=true;space.notify_all();ready.notify_all();}
    }
};

SpikeEventArchiveWriter::SpikeEventArchiveWriter():d(new Impl) {d->status.writerFinished=true;}
SpikeEventArchiveWriter::~SpikeEventArchiveWriter(){finish(SpikeArchiveState::Cancelled);wait();}
bool SpikeEventArchiveWriter::begin(const QString &directory,const QUuid &runId,const QJsonObject &metadata,const SpikeArchiveOptions &options,QString *error) {
    std::lock_guard<std::mutex> lock(d->mutex);
    if(d->status.state!=SpikeArchiveState::Idle){setError(error,"Archive writer already used");return false;}
    if(runId.isNull()||directory.isEmpty()||options.maxQueuedBytes<256||options.maxQueuedBytes>1024LL*1024*1024||
       options.testWriteDelayMs<0||!jsonWithinLimit(metadata)){setError(error,"Invalid archive options or run metadata");return false;}
    QDir dir(directory);
    if(dir.exists()&&!dir.entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).isEmpty()){
        setError(error,"Archive sidecar directory must be new or empty");return false;
    }
    if(!QDir().mkpath(directory)){setError(error,"Cannot create archive sidecar directory");return false;}
    QFile f(eventsPath(directory));Encoder e;e.json(metadata);auto initial=header(runId)+frame(RunRecord,1,e.b);
    if(!f.open(QIODevice::WriteOnly|QIODevice::NewOnly)||f.write(initial)!=initial.size()||!f.flush()){setError(error,f.errorString());return false;}f.close();
    d->dir=dir.absolutePath();d->options=options;d->status.runId=runId;d->status.state=SpikeArchiveState::Running;d->status.verifiedBytes=initial.size();
    if(!atomicJson(dir.filePath("archive.json"),statusJson(d->status),error)){d->status.state=SpikeArchiveState::Failed;return false;}
    d->status.writerFinished=false;
    d->worker=std::thread([this]{d->run();});return true;
}
SpikeArchiveEnqueueResult SpikeEventArchiveWriter::enqueueRevision(quint64 detector,quint64 rule,const QJsonObject &metadata,SpikeArchiveEnqueueMode mode,const std::atomic_bool *cancel) {
    std::unique_lock<std::mutex> lock(d->mutex);
    if(d->status.state!=SpikeArchiveState::Running||d->finishing)return d->status.state==SpikeArchiveState::Failed?SpikeArchiveEnqueueResult::Failed:SpikeArchiveEnqueueResult::NotRunning;
    if(!jsonWithinLimit(metadata)){d->failLocked("Revision metadata exceeds eight MiB");return SpikeArchiveEnqueueResult::Failed;}
    Encoder e;e.u64(detector);e.u64(rule);e.json(metadata);
    const RevisionKey key{detector,rule};const auto digest=QCryptographicHash::hash(e.b,QCryptographicHash::Sha256);
    if(d->revisions.contains(key)){if(d->revisions.value(key)==digest)return SpikeArchiveEnqueueResult::Accepted;d->failLocked("Revision metadata cannot change under an existing revision pair");return SpikeArchiveEnqueueResult::Failed;}
    auto result=d->reserve(lock,e.b.size()+48,mode,cancel);if(result!=SpikeArchiveEnqueueResult::Accepted)return result;
    if(d->revisions.contains(key)){if(d->revisions.value(key)==digest)return result;d->failLocked("Revision metadata cannot change under an existing revision pair");return SpikeArchiveEnqueueResult::Failed;}
    if(d->revisions.size()>=MaxRegistrations){d->failLocked("Archive revision registry limit reached");return SpikeArchiveEnqueueResult::Failed;}
    d->revisions.insert(key,digest);d->push(frame(RevisionRecord,++d->ordinal,e.b));return result;
}
SpikeArchiveEnqueueResult SpikeEventArchiveWriter::enqueueBatch(const QVector<SpikeArchiveRecord> &records,SpikeArchiveEnqueueMode mode,const std::atomic_bool *cancel) {
    std::unique_lock<std::mutex> lock(d->mutex);
    if(d->status.state!=SpikeArchiveState::Running||d->finishing)return d->status.state==SpikeArchiveState::Failed?SpikeArchiveEnqueueResult::Failed:SpikeArchiveEnqueueResult::NotRunning;
    if(records.isEmpty())return SpikeArchiveEnqueueResult::Accepted;
    if(records.size()>MaxBatch){d->failLocked("Archive batch exceeds 4096 events");return SpikeArchiveEnqueueResult::Failed;}
    Encoder e;e.u32(quint32(records.size()));QString error;quint64 last=0;
    for(const auto &r:records){
        if(!validEvent(r,d->status.runId,&error)||(last&&r.event.eventId<=last)){d->failLocked(error.isEmpty()?"Batch event IDs are not strictly increasing":error);return SpikeArchiveEnqueueResult::Failed;}
        encodeEvent(e,r);last=r.event.eventId;if(e.b.size()>MaxPayload){d->failLocked("Archive batch exceeds 16 MiB");return SpikeArchiveEnqueueResult::Failed;}
    }
    auto result=d->reserve(lock,e.b.size()+48,mode,cancel);if(result!=SpikeArchiveEnqueueResult::Accepted)return result;
    if(d->revisions.isEmpty()||records.first().event.eventId<=d->status.lastEventId){d->failLocked("Missing revision metadata or reused event identity");return SpikeArchiveEnqueueResult::Failed;}
    auto ranges=d->eventRanges;
    for(const auto &r:records){if(!d->revisions.contains({r.event.detectorRevision,r.event.ruleRevision})){d->failLocked("Event references unregistered revision metadata");return SpikeArchiveEnqueueResult::Failed;}
        if(!addEventRange(ranges,r.event.eventId,r.event.eventId)){d->failLocked("Archive sparse event identity registry limit reached");return SpikeArchiveEnqueueResult::Failed;}}
    d->eventRanges=std::move(ranges);
    d->push(frame(EventsRecord,++d->ordinal,e.b),quint64(records.size()),last);return result;
}
bool SpikeEventArchiveWriter::finish(SpikeArchiveState terminal,const QJsonObject &sourceIntegrity) {
    std::lock_guard<std::mutex> lock(d->mutex);
    if(d->status.state!=SpikeArchiveState::Running||d->finishing)return false;
    if((!terminalState(terminal)&&terminal!=SpikeArchiveState::Failed)||!jsonWithinLimit(sourceIntegrity))return false;
    auto proposed=d->status;proposed.state=terminal;proposed.sourceIntegrity=sourceIntegrity;
    proposed.writtenEvents=proposed.acceptedEvents;
    if(terminal==SpikeArchiveState::Failed)proposed.error=sourceIntegrity.value("error").toString();
    // Leave room for final byte-count/checksum fields in the compact manifest
    // and finish marker, including a future count growing to uint64's width.
    if(QJsonDocument(statusJson(proposed)).toJson(QJsonDocument::Compact).size()>MaxJson-2048)return false;
    d->requestedTerminal=terminal;d->status.sourceIntegrity=sourceIntegrity;
    if(terminal==SpikeArchiveState::Failed){
        const auto reason=sourceIntegrity.value("error").toString();
        d->failLocked(reason.isEmpty()?QStringLiteral("Archive run explicitly failed by the processing pipeline"):reason);
    }else{d->finishing=true;d->ready.notify_all();d->space.notify_all();}
    return true;
}
void SpikeEventArchiveWriter::wait(){if(d->worker.joinable())d->worker.join();}
SpikeArchiveStatus SpikeEventArchiveWriter::status() const {std::lock_guard<std::mutex> lock(d->mutex);return d->status;}
QString SpikeEventArchiveWriter::directory() const {std::lock_guard<std::mutex> lock(d->mutex);return d->dir;}

namespace {
QByteArray annotationPayload(const SpikeArchiveAnnotation &a) {
    Encoder e;e.b+=a.runId.toRfc4122();e.u64(a.eventId);e.i32(a.unitId);e.u64(a.annotationId);
    e.bytes(a.utc.toUtf8());e.bytes(a.note.toUtf8());return e.b;
}
bool validAnnotation(const SpikeArchiveAnnotation &a,const QUuid &run,QString *error) {
    if(a.runId!=run||a.eventId==0||a.unitId < -1||a.note.toUtf8().size()>4096){setError(error,"Invalid manual annotation identity, unit, or note length");return false;}return true;
}
bool decodeAnnotation(const QByteArray &b,const QUuid &run,SpikeArchiveAnnotation *a,QString *error) {
    Decoder d{b};if(!d.need(16)){setError(error,"Truncated annotation");return false;}
    a->runId=QUuid::fromRfc4122(b.left(16));d.p=16;a->eventId=d.u64();a->unitId=d.i32();a->annotationId=d.u64();
    a->utc=QString::fromUtf8(d.bytes(128));a->note=QString::fromUtf8(d.bytes(4096));
    if(!d.done()||a->annotationId==0||!QDateTime::fromString(a->utc,Qt::ISODateWithMs).isValid()){setError(error,"Malformed annotation record");return false;}
    return validAnnotation(*a,run,error);
}
bool u64Property(const QJsonObject &o,const char *key,quint64 *n) {
    bool ok=false;*n=o.value(QLatin1String(key)).toString().toULongLong(&ok);return ok;
}
bool scanAnnotationTail(const QString &dir,const QUuid &run,quint64 *lastId,qint64 *validEnd,QString *error,const std::atomic_bool *cancel) {
    QFile f(annotationsPath(dir));*validEnd=HeaderBytes;
    if(!f.exists())return true;if(!f.open(QIODevice::ReadOnly)){setError(error,f.errorString());return false;}
    QUuid actual;if(!readHeader(f,&actual,error)||actual!=run){setError(error,"Annotation sidecar run identity mismatch");return false;}
    quint64 ordinal=0;
    while(f.pos()<f.size()){
        Frame rec;SpikeArchiveAnnotation a;
        if(!readFrame(f,f.size(),&rec,error,cancel)||rec.kind!=AnnotationRecord||rec.ordinal!=++ordinal||
           !decodeAnnotation(rec.payload,run,&a,error)||a.annotationId<=*lastId){setError(error,"Annotation sidecar has an invalid tail; preserved without appending");return false;}
        *lastId=a.annotationId;*validEnd=rec.end;
    }return true;
}
}

SpikeArchiveEnqueueResult SpikeEventArchiveWriter::enqueueManualLabels(const QVector<SpikeArchiveAnnotation> &labels,SpikeArchiveEnqueueMode mode,const std::atomic_bool *cancel) {
    std::unique_lock<std::mutex> lock(d->mutex);
    if(d->status.state!=SpikeArchiveState::Running||d->finishing)return d->status.state==SpikeArchiveState::Failed?SpikeArchiveEnqueueResult::Failed:SpikeArchiveEnqueueResult::NotRunning;
    if(labels.isEmpty())return SpikeArchiveEnqueueResult::Accepted;
    if(labels.size()>2048){d->failLocked("Manual annotation batch exceeds 2048 labels");return SpikeArchiveEnqueueResult::Failed;}
    QString error;QByteArray bytes;quint64 ordinal=d->ordinal,id=d->annotationId;
    const auto utc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for(auto label:labels){
        if(!validAnnotation(label,d->status.runId,&error)||!containsEvent(d->eventRanges,label.eventId)){d->failLocked(error.isEmpty()?"Manual annotation references an unaccepted event":error);return SpikeArchiveEnqueueResult::Failed;}
        label.annotationId=++id;label.utc=utc;bytes+=frame(AnnotationRecord,++ordinal,annotationPayload(label));
    }
    auto result=d->reserve(lock,bytes.size(),mode,cancel);if(result!=SpikeArchiveEnqueueResult::Accepted)return result;
    // Waiting may let another producer insert records; regenerate the small frame
    // ordinals instead of committing stale values into the shared sequence.
    if(ordinal!=d->ordinal+quint64(labels.size())||id!=d->annotationId+quint64(labels.size())){
        bytes.clear();ordinal=d->ordinal;id=d->annotationId;
        for(auto label:labels){label.annotationId=++id;label.utc=utc;bytes+=frame(AnnotationRecord,++ordinal,annotationPayload(label));}
    }
    d->ordinal=ordinal;d->annotationId=id;d->push(std::move(bytes));return result;
}

bool SpikeEventArchiveReader::open(const QString &directory,QString *error,const std::atomic_bool *cancel) {
    m_directory.clear();m_metadata={};m_status={};m_annotationVerifiedBytes=HeaderBytes;m_annotationRecoveryError.clear();
    auto cancelled=[&]{if(!cancel||!cancel->load())return false;setError(error,"Archive read cancelled");m_directory.clear();return true;};
    if(cancelled())return false;
    QFile f(eventsPath(directory));if(!f.open(QIODevice::ReadOnly)){setError(error,f.errorString());return false;}
    QUuid run;if(!readHeader(f,&run,error))return false;
    m_directory=QDir(directory).absolutePath();m_status.runId=run;m_status.state=SpikeArchiveState::Running;m_status.verifiedBytes=HeaderBytes;
    QHash<RevisionKey,QByteArray> revisions;EventRanges ranges;quint64 ordinal=0,lastAnnotation=0;
    bool haveRun=false,terminal=false;QByteArray terminalDigest;QJsonObject terminalJson;
    QString recovery;
    const qint64 size=f.size();
    while(f.pos()<size){
        if(cancelled())return false;
        Frame rec;if(!readFrame(f,size,&rec,&recovery,cancel)){if(cancelled())return false;break;}
        if(rec.ordinal!=ordinal+1||terminal){recovery="Invalid record order or data after terminal record";break;}
        bool valid=true;
        if(rec.kind==RunRecord){
            Decoder d{rec.payload};auto metadata=d.json();valid=!haveRun&&ordinal==0&&d.done();if(valid){haveRun=true;m_metadata=metadata;}
        }else if(!haveRun){valid=false;
        }else if(rec.kind==RevisionRecord){
            Decoder d{rec.payload};const auto detector=d.u64(),rule=d.u64();d.json();const RevisionKey key{detector,rule};
            auto digest=QCryptographicHash::hash(rec.payload,QCryptographicHash::Sha256);
            valid=d.done()&&revisions.size()<MaxRegistrations&&!revisions.contains(key);if(valid)revisions.insert(key,digest);
        }else if(rec.kind==EventsRecord){
            QVector<SpikeArchiveRecord> events;valid=decodeEvents(rec.payload,run,&events,&recovery);
            quint64 last=m_status.lastEventId;auto nextRanges=ranges;
            if(valid)for(const auto &event:events){const auto &e=event.event;
                if(e.eventId<=last||!revisions.contains({e.detectorRevision,e.ruleRevision})||!addEventRange(nextRanges,e.eventId,e.eventId)){valid=false;break;}last=e.eventId;
            }
            if(valid){m_status.writtenEvents+=quint64(events.size());m_status.lastEventId=last;ranges=std::move(nextRanges);}
        }else if(rec.kind==AnnotationRecord){
            SpikeArchiveAnnotation a;valid=decodeAnnotation(rec.payload,run,&a,&recovery)&&a.annotationId>lastAnnotation&&containsEvent(ranges,a.eventId);if(valid)lastAnnotation=a.annotationId;
        }else if(rec.kind==TerminalRecord){
            Decoder d{rec.payload};auto obj=d.json();quint64 written=0,accepted=0,last=0;auto state=parseState(obj["archive_state"].toString());
            valid=d.done()&&terminalState(state)&&QUuid(obj["run_id"].toString())==run&&u64Property(obj,"written_events",&written)&&
                u64Property(obj,"accepted_events",&accepted)&&u64Property(obj,"last_event_id",&last)&&written==m_status.writtenEvents&&accepted==written&&last==m_status.lastEventId;
            if(valid){terminal=true;terminalDigest=rec.digest;terminalJson=obj;m_status.sourceIntegrity=obj["source_integrity"].toObject();}
        }else valid=false;
        if(!valid){if(recovery.isEmpty())recovery="Invalid archive record semantics or unregistered revision";break;}
        ordinal=rec.ordinal;m_status.verifiedBytes=rec.end;
    }
    m_status.acceptedEvents=m_status.writtenEvents;
    auto manifest=readJson(QDir(directory).filePath("archive.json"));auto finish=readJson(QDir(directory).filePath("finish.json"));
    if(terminal&&recovery.isEmpty()&&m_status.verifiedBytes==size&&QUuid(finish["run_id"].toString())==run&&
       finish["terminal_sha256"].toString()==QString::fromLatin1(terminalDigest.toHex())&&
       finish["verified_bytes"].toString()==QString::number(size)&&finish["archive_state"]==terminalJson["archive_state"]&&
       finish["written_events"]==terminalJson["written_events"]&&finish["accepted_events"]==terminalJson["accepted_events"]&&
       manifest["archive_state"]==terminalJson["archive_state"]&&QUuid(manifest["run_id"].toString())==run){
        m_status.state=parseState(terminalJson["archive_state"].toString());m_status.finishVerified=true;
    }else{
        m_status.recoveredPrefix=true;
        if(!recovery.isEmpty()){m_status.state=SpikeArchiveState::Failed;m_status.error=recovery;}
        else if(QUuid(manifest["run_id"].toString())==run&&manifest["archive_state"].toString()=="failed"){
            m_status.state=SpikeArchiveState::Failed;m_status.error=manifest["error"].toString();m_status.sourceIntegrity=manifest["source_integrity"].toObject();
        }else{m_status.state=SpikeArchiveState::Running;m_status.error="No verified finish marker; only the verified prefix is available";}
    }
    quint64 accepted=0;
    if(QUuid(manifest["run_id"].toString())==run&&u64Property(manifest,"accepted_events",&accepted)&&accepted>=m_status.writtenEvents)
        m_status.acceptedEvents=accepted;
    if(!haveRun){m_status.state=SpikeArchiveState::Failed;m_status.recoveredPrefix=true;if(m_status.error.isEmpty())m_status.error="Missing run metadata";}
    // Verify annotation history against the exact event prefix, without keeping
    // per-event or per-annotation indexes. A healthy event finish remains valid
    // even if only the separately appended annotation tail requires recovery.
    QFile annotations(annotationsPath(directory));
    if(annotations.exists()){
        QUuid annotationRun;
        if(!annotations.open(QIODevice::ReadOnly)||!readHeader(annotations,&annotationRun,&m_annotationRecoveryError)||annotationRun!=run){
            if(m_annotationRecoveryError.isEmpty())m_annotationRecoveryError="Invalid annotation sidecar header/run identity";
        }else{
            quint64 annotationOrdinal=0;const auto bound=annotations.size();
            while(annotations.pos()<bound){
                if(cancelled())return false;
                Frame rec;SpikeArchiveAnnotation annotation;
                if(!readFrame(annotations,bound,&rec,&m_annotationRecoveryError,cancel)){if(cancelled())return false;break;}
                if(rec.kind!=AnnotationRecord||rec.ordinal!=annotationOrdinal+1||
                   !decodeAnnotation(rec.payload,run,&annotation,&m_annotationRecoveryError)||
                   annotation.annotationId<=lastAnnotation||!containsEvent(ranges,annotation.eventId)){
                    if(m_annotationRecoveryError.isEmpty())m_annotationRecoveryError="Invalid annotation order or unknown event identity";break;
                }
                lastAnnotation=annotation.annotationId;annotationOrdinal=rec.ordinal;m_annotationVerifiedBytes=rec.end;
            }
        }
    }
    if(cancelled())return false;
    return true;
}

SpikeArchivePage SpikeEventArchiveReader::readPage(const SpikeArchiveQuery &query,SpikeArchiveCursor cursor,int limit,qint64 scanBudgetBytes) const {
    SpikeArchivePage page;page.next=cursor;
    if(m_directory.isEmpty()){page.error="No archive is open";return page;}
    if(limit<1||limit>MaxPage||scanBudgetBytes<1||cursor.byteOffset<HeaderBytes||cursor.byteOffset>m_status.verifiedBytes||
       !std::isfinite(query.sourceTimeFrom)||!std::isfinite(query.sourceTimeTo)||query.electrode < -1){page.error="Invalid bounded page query or cursor";return page;}
    QFile f(eventsPath(m_directory));if(!f.open(QIODevice::ReadOnly)||!f.seek(cursor.byteOffset)){page.error=f.errorString();return page;}
    qint64 scanned=0;
    while(f.pos()<m_status.verifiedBytes&&scanned<scanBudgetBytes){
        Frame rec;if(!readFrame(f,m_status.verifiedBytes,&rec,&page.error))return page;scanned+=rec.end-rec.start;
        if(rec.kind==EventsRecord){
            QVector<SpikeArchiveRecord> records;if(!decodeEvents(rec.payload,m_status.runId,&records,&page.error))return page;
            if(cursor.itemOffset>quint32(records.size())){page.error="Invalid intra-batch page cursor";return page;}
            for(quint32 i=cursor.itemOffset;i<quint32(records.size());++i){
                if(matches(records[int(i)].event,query))page.events.append(std::move(records[int(i)]));
                if(page.events.size()==limit){page.next={rec.start,i+1};if(i+1==quint32(records.size()))page.next={rec.end,0};page.atEnd=page.next.byteOffset==m_status.verifiedBytes;return page;}
            }
        }else if(cursor.itemOffset!=0){page.error="Intra-batch cursor does not point at an event record";return page;}
        cursor.itemOffset=0;page.next={rec.end,0};
    }
    page.atEnd=page.next.byteOffset==m_status.verifiedBytes;return page;
}

bool SpikeEventArchiveReader::revisionMetadata(quint64 detector,quint64 rule,QJsonObject *metadata,QString *error) const {
    if(!metadata||m_directory.isEmpty()){setError(error,"No open archive or output metadata");return false;}
    QFile f(eventsPath(m_directory));if(!f.open(QIODevice::ReadOnly)||!f.seek(HeaderBytes)){setError(error,f.errorString());return false;}
    while(f.pos()<m_status.verifiedBytes){Frame rec;if(!readFrame(f,m_status.verifiedBytes,&rec,error))return false;
        if(rec.kind==RevisionRecord){Decoder d{rec.payload};auto dr=d.u64(),rr=d.u64();auto obj=d.json();if(!d.done()){setError(error,"Invalid revision metadata");return false;}
            if(dr==detector&&rr==rule){*metadata=obj;return true;}}
    }
    setError(error,"Requested revision pair not found in verified archive prefix");return false;
}

SpikeArchiveAnnotationPage SpikeEventArchiveReader::readAnnotations(quint64 eventId,qint64 offset,int limit) const {
    SpikeArchiveAnnotationPage page;page.nextOffset=offset;
    if(m_directory.isEmpty()||limit<1||limit>MaxPage||offset==std::numeric_limits<qint64>::min()) {page.error="Invalid annotation page request";return page;}
    bool sidecar=offset<0;qint64 position=sidecar?(-offset-1):offset;
    if(position<HeaderBytes){page.error="Invalid annotation cursor";return page;}
    qint64 budget=64*1024*1024;
    while(true){
        QFile f(sidecar?annotationsPath(m_directory):eventsPath(m_directory));
        if(sidecar&&!f.exists()){page.atEnd=true;return page;}
        if(!f.open(QIODevice::ReadOnly)){page.error=f.errorString();return page;}
        QUuid run;if(!readHeader(f,&run,&page.error)||run!=m_status.runId){page.error="Annotation run identity mismatch";page.recoveredPrefix=sidecar;return page;}
        const qint64 bound=sidecar?m_annotationVerifiedBytes:m_status.verifiedBytes;
        if(position>bound||!f.seek(position)){page.error="Annotation cursor exceeds verified data";return page;}
        while(f.pos()<bound&&budget>0){Frame rec;
            if(!readFrame(f,bound,&rec,&page.error)){page.recoveredPrefix=true;page.atEnd=true;return page;}budget-=rec.end-rec.start;
            page.nextOffset=sidecar?(-rec.end-1):rec.end;
            if(rec.kind==AnnotationRecord){SpikeArchiveAnnotation a;if(!decodeAnnotation(rec.payload,m_status.runId,&a,&page.error)){page.recoveredPrefix=true;page.atEnd=true;return page;}
                if(eventId==0||a.eventId==eventId)page.annotations.append(std::move(a));
                if(page.annotations.size()==limit)return page;
            }else if(sidecar){page.error="Unexpected record in annotation sidecar";page.recoveredPrefix=true;page.atEnd=true;return page;}
        }
        if(f.pos()<bound)return page;
        if(sidecar){page.atEnd=true;if(!m_annotationRecoveryError.isEmpty()){page.recoveredPrefix=true;page.error=m_annotationRecoveryError;}return page;}
        sidecar=true;position=HeaderBytes;page.nextOffset=-HeaderBytes-1;
        if(budget<=0)return page;
    }
}

bool SpikeEventArchiveReader::appendManualLabel(const QString &directory,const QUuid &runId,quint64 eventId,int unitId,const QString &note,QString *error,const std::atomic_bool *cancel) {
    SpikeArchiveAnnotation a;a.runId=runId;a.eventId=eventId;a.unitId=unitId;a.note=note;return appendManualLabels(directory,{a},error,cancel);
}
bool SpikeEventArchiveReader::appendManualLabels(const QString &directory,const QVector<SpikeArchiveAnnotation> &labels,QString *error,const std::atomic_bool *cancel) {
    auto cancelled=[&]{if(!cancel||!cancel->load())return false;setError(error,"Annotation append cancelled before commit");return true;};
    if(cancelled())return false;
    if(labels.isEmpty()||labels.size()>2048){setError(error,"Manual annotation batch must contain 1 to 2048 labels");return false;}
    QLockFile lock(QDir(directory).filePath("annotations.lock"));lock.setStaleLockTime(30000);
    const auto lockStart=std::chrono::steady_clock::now();
    while(!lock.tryLock(0)){
        if(cancelled())return false;
        if(std::chrono::steady_clock::now()-lockStart>=std::chrono::seconds(1)){setError(error,"Another annotation writer holds the sidecar lock");return false;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    SpikeEventArchiveReader reader;if(!reader.open(directory,error,cancel))return false;
    if(!reader.m_annotationRecoveryError.isEmpty()){setError(error,reader.m_annotationRecoveryError);return false;}
    if(!reader.status().finishVerified){setError(error,"Use the active writer for labels until the archive has a verified terminal state");return false;}
    QMap<quint64,bool> wanted;
    for(const auto &a:labels){if(!validAnnotation(a,reader.status().runId,error))return false;wanted.insert(a.eventId,false);}
    quint64 lastAnnotation=0;
    QFile f(eventsPath(directory));if(!f.open(QIODevice::ReadOnly)||!f.seek(HeaderBytes)){setError(error,f.errorString());return false;}
    while(f.pos()<reader.status().verifiedBytes){if(cancelled())return false;Frame rec;if(!readFrame(f,reader.status().verifiedBytes,&rec,error,cancel))return false;
        if(rec.kind==EventsRecord){QVector<SpikeArchiveRecord> records;if(!decodeEvents(rec.payload,reader.status().runId,&records,error))return false;
            for(const auto &r:records)if(wanted.contains(r.event.eventId))wanted[r.event.eventId]=true;
        }else if(rec.kind==AnnotationRecord){SpikeArchiveAnnotation a;if(!decodeAnnotation(rec.payload,reader.status().runId,&a,error))return false;lastAnnotation=std::max(lastAnnotation,a.annotationId);}
    }
    for(auto it=wanted.cbegin();it!=wanted.cend();++it)if(!it.value()){setError(error,"Manual annotation event ID not found in verified archive");return false;}
    qint64 validEnd=0;
    if(!scanAnnotationTail(directory,reader.status().runId,&lastAnnotation,&validEnd,error,cancel))return false;
    QFile aFile(annotationsPath(directory));const bool exists=aFile.exists();
    // The sidecar ordinal is independent of annotation IDs, which continue the
    // active archive's ID sequence. Count framing records without storing them.
    quint64 ordinal=0;
    if(exists){QFile count(annotationsPath(directory));if(!count.open(QIODevice::ReadOnly)||!count.seek(HeaderBytes)){setError(error,count.errorString());return false;}
        while(count.pos()<validEnd){if(cancelled())return false;Frame rec;if(!readFrame(count,validEnd,&rec,error,cancel))return false;++ordinal;}}
    QByteArray bytes;const auto utc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for(auto a:labels){if(cancelled())return false;a.utc=utc;a.annotationId=++lastAnnotation;bytes+=frame(AnnotationRecord,++ordinal,annotationPayload(a));}
    if(cancelled())return false;
    // Commit phase: cancellation cannot interrupt a bounded append and leave an
    // intentionally half-written label. Errors remain recoverable framed tails.
    if(!aFile.open(QIODevice::WriteOnly|QIODevice::Append)){setError(error,aFile.errorString());return false;}
    if(!exists)bytes.prepend(header(reader.status().runId));
    if(aFile.write(bytes)!=bytes.size()||!aFile.flush()){setError(error,aFile.errorString());return false;}return true;
}

} // namespace ccv2
