#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <cstdio>
#include "network/replay_controller.h"
#include "service/spike_detect_worker.h"
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);if(argc!=2)return 2;
 auto replayQ=std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(8);ccv2::ReplayController replay(replayQ);replay.setTimelineEpoch(23);
 if(!replay.open(QString::fromLocal8Bit(argv[1]),1000000))return 3;replay.play();QElapsedTimer t;t.start();while(replayQ->size()==0&&t.elapsed()<5000){QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);}replay.pause();QByteArray bytes;ccv2::StreamBlockInfo info;if(!replayQ->pop(bytes,0,nullptr,&info))return 4;replay.close();
 const qint64 n=bytes.size()/1024;auto q=std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(2);q->push(bytes,false,nullptr,info);
 ccv2::SpikeSnippetStore store;if(!store.configure(256,33,16))return 5;store.resetTimeline(23);std::atomic<quint64> epoch{23};std::atomic_bool stop{false};
 ccv2::SpikeDetectConfig cfg;cfg.proc.sampleRate=20000;cfg.inputGain=1;cfg.tdmEnabled=false;cfg.proc.notchHz=0;cfg.proc.highpassHz=250;cfg.proc.spikeLowpassHz=5000;
 ccv2::SpikeDetectWorker worker(q,&store,&stop,cfg,&epoch);worker.start();QVector<qint64>exposure;ccv2::SpikeAnalysisQuality quality;t.restart();
 do{store.snapshotAnalysis(&exposure,&quality);if(exposure.value(255)==n)break;QThread::msleep(1);}while(t.elapsed()<5000);
 stop.store(true);q->wakeAll();bool stopped=worker.wait(5000);store.snapshotAnalysis(&exposure,&quality);
 bool pass=stopped&&n>0&&exposure.size()==256&&quality.unverifiedFrames==n&&quality.invalidFrames==0&&quality.incomplete();for(auto e:exposure)pass=pass&&e==n;
 QJsonObject row{{"kind","sample_detector_unverified_status"},{"passed",pass},{"sample_frames",n},{"source_begin",info.firstFrame()},{"source_end_inclusive",info.frameIndices.last()},{"unverified_frames",quality.unverifiedFrames},{"invalid_frames",quality.invalidFrames},{"quality_incomplete",quality.incomplete()},{"every_lane_exposure_equals_input_frames",pass},{"input_validity_mask_empty",info.frameValid.isEmpty()},{"input_integrity_unknown",info.integrityUnknown},{"assumed_processing_rate_hz",20000},{"assumed_gain",1},{"assumed_tdm",false},{"note","Bookkeeping only; these assumed settings do not establish physiological signal, spike, timing or amplitude accuracy."}};
 puts(QJsonDocument(row).toJson(QJsonDocument::Compact).constData());return pass?0:1;
}
