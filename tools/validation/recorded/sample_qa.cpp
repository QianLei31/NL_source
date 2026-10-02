#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <functional>
#include "core/constants.h"
#include "core/frame_timestamp_reconciler.h"
#include "network/replay_controller.h"
#include "network/timestamp_checker.h"
#include "io/session_manifest.h"
using namespace ccv2;
static QJsonObject statsJson(const TimestampContinuityStats&s){
 return {{"frames_seen",s.framesSeen},{"transitions_checked",s.transitionsChecked},{"discontinuities",s.discontinuities},{"estimated_missing_frames",s.estimatedMissingFrames},{"repeated_timestamps",s.repeatedFrames},{"irregular_jumps",s.irregularJumps},{"intra_frame_mismatch_frames",s.intraFrameMismatchFrames},{"mismatched_channel_words",s.mismatchedChannelWords},{"first_error_frame",s.firstErrorFrame},{"first_error_expected",(qint64)s.firstErrorExpected},{"first_error_actual",(qint64)s.firstErrorActual},{"first_timestamp",(qint64)s.firstTimestamp},{"last_timestamp",(qint64)s.lastTimestamp},{"expected_step",(qint64)s.expectedStep},{"calibrated",s.calibrated}};
}
static bool spin(const std::function<bool()>& done,int timeoutMs=10000){QElapsedTimer t;t.start();while(!done()&&t.elapsed()<timeoutMs){QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);}return done();}
static void emitJson(const char *kind,QJsonObject result){result["kind"]=kind;auto b=QJsonDocument(result).toJson(QJsonDocument::Compact);puts(b.constData());fflush(stdout);}
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);if(argc!=2)return 2;QString path=QString::fromLocal8Bit(argv[1]);QFile f(path);if(!f.open(QIODevice::ReadOnly))return 3;
 TimestampContinuityAnalyzer fixed(1),autodetect(0),fragmented(0);fixed.setIntraFrameStride(1);autodetect.setIntraFrameStride(1);fragmented.setIntraFrameStride(1);
 FrameTimestampReconciler timeline;timeline.reset(0);QCryptographicHash full(QCryptographicHash::Sha256),complete(QCryptographicHash::Sha256);
 qint64 frames=0,words=0,nonzeroTs=0,repeatedPayload=0,allzeroFrames=0,reconcilerGaps=0;quint32 minTs=0xFFFFF,maxTs=0;unsigned minCode=4095,maxCode=0;std::array<qint64,4096>hist{};std::array<unsigned,256>mins,maxs;mins.fill(4095);maxs.fill(0);QByteArray previous;QByteArray tail;
 while(!f.atEnd()){
  QByteArray b=f.read(4096LL*kFrameBytes);full.addData(b);fixed.process(b);autodetect.process(b);for(qsizetype pos=0;pos<b.size();pos+=65537)fragmented.process(b.mid(pos,65537));
  qsizetype usable=(b.size()/kFrameBytes)*kFrameBytes;complete.addData(b.constData(),usable);if(usable!=b.size())tail=b.mid(usable);
  for(qsizetype off=0;off<usable;off+=kFrameBytes){const char*p=b.constData()+off;bool zero=true;if(previous.size()==kFrameBytes&&!memcmp(previous.constData(),p,kFrameBytes))++repeatedPayload;previous=QByteArray(p,kFrameBytes);
   qint64 gap=0;auto idx=timeline.advance(qFromLittleEndian<quint32>(p)>>12,&gap);reconcilerGaps+=gap;if(idx!=frames+reconcilerGaps)return 4;
   for(int c=0;c<256;++c){auto raw=qFromLittleEndian<quint32>(p+4*c);auto ts=raw>>12;unsigned code=raw&4095;zero=zero&&(raw==0);++words;nonzeroTs+=(ts!=0);minTs=std::min(minTs,ts);maxTs=std::max(maxTs,ts);minCode=std::min(minCode,code);maxCode=std::max(maxCode,code);mins[c]=std::min(mins[c],code);maxs[c]=std::max(maxs[c],code);++hist[code];}
   allzeroFrames+=zero;++frames;
  }
 }
 auto completeHash=complete.result().toHex();QJsonArray channelRange;for(int c=0;c<256;++c)channelRange.append(QJsonObject{{"channel",c},{"min",(int)mins[c]},{"max",(int)maxs[c]}});int usedCodes=0;for(auto n:hist)usedCodes+=(n>0);
 emitJson("whole_file",{{"bytes",f.size()},{"sha256",QString(full.result().toHex())},{"complete_bytes_sha256",QString(completeHash)},{"complete_frames",frames},{"complete_words",words},{"tail_bytes",tail.size()},{"tail_full_words",tail.size()/4},{"nonzero_timestamp_words",nonzeroTs},{"timestamp_min",(qint64)minTs},{"timestamp_max",(qint64)maxTs},{"adc_code_min",(int)minCode},{"adc_code_max",(int)maxCode},{"adc_unique_codes",usedCodes},{"code_zero_count",hist[0]},{"code_4095_count",hist[4095]},{"all_zero_frames",allzeroFrames},{"consecutive_identical_payload_frames",repeatedPayload},{"reconciler_inferred_gap_count",reconcilerGaps},{"reconciler_next_index",timeline.nextIndex()},{"reconciler_active",timeline.active()},{"fixed_step_1",statsJson(fixed.stats())},{"auto_step",statsJson(autodetect.stats())},{"fragmented_65537_byte_input_same_stats",statsJson(autodetect.stats())==statsJson(fragmented.stats())},{"per_channel_adc_ranges",channelRange}});
 SessionInput input;QString error;bool resolved=SessionManifest::resolveInput(path,20000.,&input,&error);emitJson("session_resolver",{{"ok",resolved},{"error",error},{"assumed_rate_hz",20000},{"total_frames",input.totalFrames},{"tail_bytes",input.ignoredTailBytes},{"has_manifest",input.hasManifest},{"tdm_known",input.metadata.tdmKnown},{"frame_validity_known",input.frameValidityKnown},{"integrity_complete",input.integrityComplete},{"integrity_unknown",input.integrityUnknown},{"invalid_range_count",input.invalidFrameRanges.size()},{"warning",input.warning}});if(!resolved||input.totalFrames!=frames)return 5;
 auto queue=std::make_shared<ThreadSafeQueue<QByteArray>>(16);ReplayController replay(queue);bool finished=false;int finishCount=0;QString playbackError;QObject::connect(&replay,&ReplayController::finished,&app,[&](){finished=true;++finishCount;});QObject::connect(&replay,&ReplayController::errorOccurred,&app,[&](const QString&e){playbackError=e;});if(!replay.open(path,20000.))return 6;emitJson("replay_open",{{"playing",replay.isPlaying()},{"frames",replay.totalFrames()},{"assumed_rate_hz",replay.sampleRate()},{"tdm_known",replay.tdmKnown()},{"source_provenance",replay.sourceProvenance()}});
 int failures=0;auto verifySeek=[&](qint64 target,const char*label){QElapsedTimer timer;timer.start();replay.pause();replay.seekFrame(target);bool ok=spin([&](){return !replay.isSeeking();});ok=ok&&replay.currentFrame()==target&&replay.nextSourceFrame()==target&&queue->size()==0&&!replay.isPlaying()&&replay.errorString().isEmpty();qint64 latency=timer.elapsed();bool payloadOk=true,indicesOk=true,emptyMask=true,unknown=false;qint64 gotFrames=0;
  if(target<frames&&ok){replay.play();ok=spin([&](){return queue->size()>0||!playbackError.isEmpty();});replay.pause();QByteArray b;StreamBlockInfo info;if(!queue->pop(b,0,nullptr,&info))ok=false;else{f.seek(target*kFrameBytes);payloadOk=b==f.read(b.size());gotFrames=b.size()/kFrameBytes;indicesOk=info.frameIndices.size()==gotFrames;for(int n=0;n<info.frameIndices.size();++n)indicesOk=indicesOk&&(info.frameIndices[n]==target+n);emptyMask=info.frameValid.isEmpty();unknown=info.integrityUnknown;}}
  ok=ok&&payloadOk&&indicesOk;failures+=!ok;emitJson("seek",{{"label",label},{"target",target},{"passed",ok},{"indexing_ms",latency},{"delivered_frames",gotFrames},{"payload_equals_original",payloadOk},{"source_indices_correct",indicesOk},{"validity_mask_empty",emptyMask},{"block_integrity_unknown",unknown}});
 };
 verifySeek(0,"start");verifySeek(frames/2,"middle");verifySeek(frames,"end");verifySeek(frames-127,"near_end");
 // One full byte-for-byte replay at an explicitly artificial transport rate.
 replay.close();queue->clear();finished=false;playbackError.clear();if(!replay.open(path,1000000.))return 7;QCryptographicHash playedHash(QCryptographicHash::Sha256);qint64 got=0,blocks=0;bool indices=true,masks=true,unknownFlag=false;QElapsedTimer playbackTimer;playbackTimer.start();replay.play();
 auto drain=[&](){QByteArray b;StreamBlockInfo info;while(queue->pop(b,0,nullptr,&info)){auto n=b.size()/kFrameBytes;indices=indices&&(b.size()%kFrameBytes==0)&&(info.frameIndices.size()==n);for(qint64 k=0;k<info.frameIndices.size();++k)indices=indices&&(info.frameIndices[k]==got+k);masks=masks&&info.frameValid.isEmpty();unknownFlag=unknownFlag||info.integrityUnknown;playedHash.addData(b);got+=n;++blocks;}};
 bool eof=spin([&](){drain();return finished||!playbackError.isEmpty();},15000);drain();bool same=playedHash.result().toHex()==completeHash;bool passed=eof&&finished&&same&&got==frames&&indices&&masks&&replay.currentFrame()==frames&&!replay.isPlaying()&&playbackError.isEmpty();failures+=!passed;
 emitJson("full_replay",{{"passed",passed},{"artificial_transport_rate_hz",1000000},{"elapsed_ms",playbackTimer.elapsed()},{"received_frames",got},{"received_blocks",blocks},{"payload_sha256_matches_complete_file",same},{"source_indices_correct",indices},{"all_validity_masks_empty",masks},{"any_block_integrity_unknown",unknownFlag},{"eof_finished_signal",finished},{"stopped_at_eof",!replay.isPlaying()},{"error",playbackError}});
 // EOF play restarts at the beginning; queued stale data must not survive.
 replay.play();bool restarted=spin([&](){return queue->size()>0||!playbackError.isEmpty();});replay.pause();QByteArray rb;StreamBlockInfo ri;restarted=restarted&&queue->pop(rb,0,nullptr,&ri)&&!ri.frameIndices.isEmpty()&&ri.frameIndices.first()==0;f.seek(0);restarted=restarted&&(rb==f.read(rb.size()));failures+=!restarted;emitJson("eof_restart",{{"passed",restarted},{"first_source_frame",ri.frameIndices.isEmpty()?-1:ri.frameIndices.first()}});replay.close();emitJson("summary",{{"failures",failures},{"passed",failures==0},{"note","Format/parser tests only. Acquisition sampling rate, TDM, gain and biological validity are unknown. Artificial replay rate is a transport test assumption."}});return failures?1:0;
}
