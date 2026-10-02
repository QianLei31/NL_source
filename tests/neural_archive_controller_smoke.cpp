#include "service/neural_archive_controller.h"
#include "service/spike_detect_worker.h"
#include "core/constants.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtEndian>
#include <iostream>
using namespace ccv2;
namespace {
int checks=0,failures=0;
void check(bool ok,const char *message){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<message<<'\n';}}
SpikeDetectConfig config(){SpikeDetectConfig c;c.proc.sampleRate=20000;c.proc.notchHz=0;c.proc.highpassHz=250;
 c.proc.spikeLowpassHz=9000;c.proc.absoluteThreshold=true;c.proc.absThresholdV=-100e-6;c.proc.refractoryMs=5;c.inputGain=60;return c;}
void push(const std::shared_ptr<ThreadSafeQueue<QByteArray>>&q,int from,int count,bool pulses){
 QByteArray bytes(count*kFrameBytes,'\0');StreamBlockInfo info;info.epoch=17;info.frameValid.fill(true,count);
 for(int i=0;i<count;++i){int f=from+i;info.frameIndices.append(f);for(int ch=0;ch<256;++ch){
 int value=2048;if(pulses&&ch==7&&f%400>=100&&f%400<124)value-=90;
 qToLittleEndian<quint32>((quint32(f)<<12)|quint32(value),bytes.data()+i*kFrameBytes+ch*4);}}
 check(q->push(bytes,false,nullptr,info),"fixture enqueue");}
bool exposure(SpikeSnippetStore &store,int n,int timeout=5000){QElapsedTimer t;t.start();while(t.elapsed()<timeout){
 QVector<qint64> samples;store.snapshotAnalysis(&samples,nullptr);if(samples.value(255)>=n)return true;QThread::msleep(1);}return false;}
void quietRevisions(const QString &path){
 auto queue=std::make_shared<ThreadSafeQueue<QByteArray>>(8);SpikeSnippetStore store;store.configure(256,33,2);store.resetTimeline(17);
 std::atomic_bool stop{false};std::atomic<quint64> epoch{17};SpikeDetectWorker worker(queue,&store,&stop,config(),&epoch);
 NeuralArchiveController controller;controller.attach(&worker);worker.start();push(queue,0,400,false);check(exposure(store,400),"quiet initial exposure");
 QString error;check(controller.start(path,{},&error),"archive starts during quiet detector");
 check(worker.setRuleSet(SpikeRuleSet::create(1,{})),"empty explicit rule revision accepted");
 push(queue,400,400,false);check(exposure(store,800),"quiet changed-rule exposure");worker.requestDrain();check(worker.wait(5000)&&worker.drained(),"quiet detector drains");
 controller.finish(SpikeArchiveState::Completed,spikeQualityJson(store));SpikeEventArchiveReader reader;
 check(reader.open(path,&error)&&reader.status().finishVerified,"quiet archive verifies");
 QJsonObject meta;check(reader.revisionMetadata(1,0,&meta,&error),"initial applied metadata survives zero events");
 check(reader.revisionMetadata(1,1,&meta,&error),"later applied rule metadata survives zero events");
 check(reader.status().writtenEvents==0,"quiet archive has no invented events");
 check(!reader.status().sourceIntegrity.value("whole_source_coverage_claimed").toBool(),"live emission interval does not claim whole input");
}
void failedRetry(const QString &root){
 auto queue=std::make_shared<ThreadSafeQueue<QByteArray>>(8);SpikeSnippetStore store;store.configure(256,33,1);store.resetTimeline(17);
 std::atomic_bool stop{false};std::atomic<quint64> epoch{17};SpikeDetectWorker worker(queue,&store,&stop,config(),&epoch);
 NeuralArchiveController controller;controller.attach(&worker);worker.start();push(queue,0,400,false);
 check(exposure(store,400),"retry initial revision exposed");
 SpikeArchiveOptions options;options.maxQueuedBytes=32768;options.testWriteDelayMs=300;QString error;
 check(controller.start(root+"/failed",{},&error,options),"slow revision writer starts");
 int cursor=400;
 for(int revision=1;revision<=4&&controller.status().state==SpikeArchiveState::Running;++revision){
   worker.setRuleSet(SpikeRuleSet::create(revision,{}));push(queue,cursor,400,false);cursor+=400;
   check(exposure(store,cursor),"quiet revision churn remains live");
 }
 check(controller.status().state==SpikeArchiveState::Failed,"live revision queue exhaustion explicit");
 const bool unfinished=!controller.status().writerFinished;
 QElapsedTimer t;t.start();const bool accepted=controller.start(root+"/retry",{},&error);
 if(unfinished)check(!accepted,"unfinished failed writer cannot be replaced");
 check(t.elapsed()<100,"failed retry never joins writer under detector callback lock");
 push(queue,cursor,400,true);check(exposure(store,cursor+400),"detection continues after failed writer");
 worker.requestDrain();check(worker.wait(5000),"failed writer worker exits");controller.finish(SpikeArchiveState::Drained);
 if(!accepted){check(controller.start(root+"/retry",{},&error),"retry succeeds after writer drain barrier");controller.finish(SpikeArchiveState::Drained);}

}
void nonblockingStop(const QString &path){
 auto queue=std::make_shared<ThreadSafeQueue<QByteArray>>(8);SpikeSnippetStore store;store.configure(256,33,1);store.resetTimeline(17);
 std::atomic_bool stop{false};std::atomic<quint64> epoch{17};SpikeDetectWorker worker(queue,&store,&stop,config(),&epoch);
 NeuralArchiveController controller;controller.attach(&worker);
 store.setAnnotationCallback([&](const QVector<SpikeEvent>&events){controller.annotate(events);});
 SpikeArchiveOptions options;options.testWriteDelayMs=250;QString error;
 check(controller.start(path,{},&error,options),"slow writer archive starts");worker.start();push(queue,0,400,true);
 check(exposure(store,400),"first event detected");check(controller.status().acceptedEvents==1,"immutable event accepted before ring eviction");
 const auto first=store.snapshotLane(7).events;QElapsedTimer timer;timer.start();
 controller.finish(SpikeArchiveState::Drained,spikeQualityJson(store),false);
 check(timer.elapsed()<100,"archive-only stop does not wait for slow disk");
 push(queue,400,400,true);check(exposure(store,800,200),"live detection continues while archive drains");
 controller.finish(SpikeArchiveState::Drained,{},false); // idempotent pending finish, not failure
 worker.requestDrain();check(worker.wait(5000)&&worker.drained(),"continued detector drains");
 controller.finish(SpikeArchiveState::Drained);SpikeEventArchiveReader reader;
 check(reader.open(path,&error)&&reader.status().finishVerified,"slow archive terminal verifies");
 check(reader.status().state==SpikeArchiveState::Drained&&reader.status().writtenEvents==1,"stop boundary excludes later events, no false failure");
 check(store.totalSpikes(7)==2&&store.snapshotLane(7).events.size()==1,"later event remains in bounded live ring only");
 // Label the archived identity directly through the same immutable annotation
 // callback path used by retained selection; this does not mutate the original.
 if(!first.isEmpty()){auto labelled=first.first();labelled.unitId=3;controller.annotate({labelled});
 QElapsedTimer wait;wait.start();while(controller.status().error.contains(QStringLiteral("正在写入"))&&wait.elapsed()<5000)QThread::msleep(2);
 check(controller.status().error.isEmpty(),"historical labels finish asynchronously");
 check(reader.open(path,&error),"reopen after label");const auto labels=reader.readAnnotations(labelled.eventId);
 check(labels.annotations.size()==1&&labels.annotations.first().unitId==3,"stable archived ID keeps append-only historical label");}
}
}
int main(int argc,char **argv){QCoreApplication app(argc,argv);QTemporaryDir temp;check(temp.isValid(),"temporary workspace");
 quietRevisions(temp.filePath("quiet"));nonblockingStop(temp.filePath("slow"));failedRetry(temp.path());
 std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
