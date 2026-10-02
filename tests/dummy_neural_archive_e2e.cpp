// Real Dummy TCP -> SessionHub -> continuous SpikePanel -> immutable sidecars.
// Rules use a deliberately broad diagnostic box: this validates mechanics and
// future-event boundaries, not biological unit isolation or accuracy claims.
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QSet>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QPointer>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "config/config_manager.h"
#include "core/constants.h"
#include "io/session_manifest.h"
#include "io/spike_event_archive.h"
#include "service/dummy_stream_server.h"
#include "service/session_hub.h"
#include "service/spike_offline_analysis.h"
#include "signal/spike_rule_classifier.h"
#include "theme/theme_manager.h"
#include "ui/spike_panel.h"
#include "ui/widgets/spike_analysis_controls.h"
#include "ui/widgets/spike_archive_browser.h"
#include "ui/widgets/spike_rule_editor.h"

using namespace ccv2;
namespace {
int checks=0;
void require(bool condition,const QString &why) {
    if(!condition) {std::cerr<<"ASSERTION FAILED: "<<why.toStdString()<<std::endl;throw std::runtime_error(why.toStdString());}
    ++checks;std::cout<<"PASS "<<why.toStdString()<<std::endl;
}
void pump(int ms) {QElapsedTimer t;t.start();do {QApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);}while(t.elapsed()<ms);}
bool until(const std::function<bool()> &p,int ms=8000) {QElapsedTimer t;t.start();while(!p()&&t.elapsed()<ms)pump(5);return p();}
template<class T>T *named(QObject *root,const char *name) {auto *v=root->findChild<T *>(QString::fromLatin1(name));if(!v)throw std::runtime_error(std::string("missing control ")+name);return v;}
QComboBox *combo(QObject *root,const QString &text) {for(auto *c:root->findChildren<QComboBox *>())if(c->findText(text)>=0)return c;throw std::runtime_error("missing combo");}
void click(QObject *root,const char *name) {auto *b=named<QPushButton>(root,name);require(until([&]{return b->isEnabled();},2500),QString("enabled control ")+name);b->click();pump(15);}
void fileAction(QPushButton *button,const QString &path) {
    QPointer<QFileDialog> prepared;bool seen=false;QElapsedTimer timeout;timeout.start();QTimer timer;
    QObject::connect(&timer,&QTimer::timeout,[&]{for(auto *w:QApplication::topLevelWidgets())if(auto *d=qobject_cast<QFileDialog *>(w);d&&d->isVisible()){
        seen=true;if(timeout.elapsed()>6000){d->reject();return;}
        if(prepared!=d){prepared=d;d->setDirectory(QFileInfo(path).absolutePath());d->selectFile(QFileInfo(path).fileName());}
        else {if(auto *e=d->findChild<QLineEdit *>("fileNameEdit"))e->setText(QFileInfo(path).fileName());static_cast<QDialog *>(d)->accept();}
    }});timer.start(20);button->click();timer.stop();pump(30);require(seen,"ordinary file chooser exercised");
}
QVector<SpikeArchiveRecord> readAll(const QString &directory,SpikeArchiveStatus *status=nullptr) {
    SpikeEventArchiveReader reader;QString error;require(reader.open(directory,&error),"independent archive reopen: "+error);
    if(status)*status=reader.status();QVector<SpikeArchiveRecord> result;SpikeArchiveCursor cursor;
    for(int page=0;page<100000;++page) {
        const auto next=reader.readPage({},cursor,128);if(!next.error.isEmpty())throw std::runtime_error(next.error.toStdString());
        require(next.events.size()<=128,"reader page remains bounded");result+=next.events;
        if(next.atEnd)return result;
        if(next.next.byteOffset==cursor.byteOffset&&next.next.itemOffset==cursor.itemOffset)throw std::runtime_error("archive cursor did not progress");
        cursor=next.next;
    }throw std::runtime_error("archive exceeded bounded test page limit");
}
void verifyRecords(const QString &path,const QVector<SpikeArchiveRecord> &records,const SpikeArchiveStatus &status) {
    require(status.finishVerified&&status.acceptedEvents==status.writtenEvents&&status.writtenEvents==quint64(records.size()),"verified terminal accounts for every accepted archive event");
    bool valid=true;quint64 previous=0;QSet<QPair<quint64,quint64>> revisions;
    for(const auto &r:records) {
        const auto &e=r.event;valid=valid&&!e.runId.isNull()&&e.runId==status.runId&&e.eventId>previous&&e.detectorRevision>0&&e.hasSourceTime()&&e.adcChannel==e.electrode&&e.sampleStride==1&&e.preSamples==8&&r.waveform.size()==33;
        for(float v:r.waveform)valid=valid&&std::isfinite(v);previous=e.eventId;revisions.insert({e.detectorRevision,e.ruleRevision});
    }
    require(valid&&!records.isEmpty(),"persistent events have stable monotonic IDs, revisions, exact ADC identity and finite paired waveforms");
    SpikeEventArchiveReader reader;QString error;require(reader.open(path,&error),"revision reader independently reopened");
    bool metadataValid=true;
    for(const auto &key:revisions){QJsonObject json;metadataValid=metadataValid&&reader.revisionMetadata(key.first,key.second,&json,&error)&&json["detector_revision"].toString()==QString::number(key.first)&&json["rule_revision"].toString()==QString::number(key.second)&&json["analysis"].toObject()["source_sample_rate_hz"].toDouble()==20000&&json["analysis"].toObject()["waveform_units"].toString()=="input_referred_volts";}
    require(metadataValid,"each saved revision resolves to the complete applied source/waveform configuration");
}
QMap<QString,QByteArray> hashFiles(const QString &folder) {
    QMap<QString,QByteArray> hashes;for(const auto &name:QDir(folder).entryList(QDir::Files,QDir::Name)){QFile f(QDir(folder).filePath(name));require(f.open(QIODevice::ReadOnly),"source file readable for immutable-input check");QCryptographicHash hash(QCryptographicHash::Sha256);require(hash.addData(&f),"source file hashed");hashes[name]=hash.result();}return hashes;
}
QMap<QString,QString> colors(const ThemePalette &p) {return {{"appBg",p.appBg.name()},{"plotBg",p.plotBg.name()},{"border",p.cardBorder.name()},{"text",p.textPrimary.name()},{"title",p.textSecondary.name()},{"axis",p.plotAxis.name()},{"wave",p.plotWave.name()},{"accent",p.primary.name()},{"grid",p.plotGrid.name()}};}
void screenshot(QWidget *widget,const QString &name,const QColor &background) {
    widget->show();pump(60);const auto image=widget->grab().toImage();require(!image.isNull()&&image.pixelColor(2,2).rgb()==background.rgb(),"actual populated "+name+" retains opaque theme background");
    const QString folder=qEnvironmentVariable("CCV2_NEURAL_ARCHIVE_QA_DIR");if(!folder.isEmpty()){QDir().mkpath(folder);require(image.save(QDir(folder).filePath(name+".png")),"saved populated "+name+" screenshot");}
}
}
int main(int argc,char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);QTemporaryDir temp;
    try {
        require(temp.isValid(),"isolated archive test directory");
        ConfigManager config(temp.filePath("config.ini"));auto cfg=config.load();
        cfg["SpikePanel"]={{"threshold_units","input_referred"},{"input_gain","60"},{"threshold_mode","1"},{"threshold_value","20"},{"retain","20"},{"notch_hz","0"},{"pre_ms","0.4"},{"post_ms","1.2"}};
        require(config.save(cfg),"bounded real detector configuration saved");
        QTcpServer reserve;require(reserve.listen(QHostAddress::LocalHost,0),"allocate loopback Dummy port");const int port=reserve.serverPort();reserve.close();
        DummyStreamServer dummy;dummy.setWaveform(DummyWaveform::Spike);QString error;require(dummy.start(port,port,&error),"built-in deterministic spike Dummy starts");
        SessionHub hub;SpikePanel panel(&config);panel.resize(1500,980);panel.setSessionHub(&hub);panel.show();panel.onActivated();
        struct Cleanup {SessionHub &hub;SpikePanel &panel;~Cleanup(){hub.stop();panel.shutdown();}} cleanup{hub,panel};
        ThemeManager theme;theme.apply("dark");panel.setWaveTheme(colors(theme.currentPalette()));
        auto *controls=panel.findChild<SpikeAnalysisControls *>();auto *editor=panel.findChild<SpikeRuleEditor *>();auto *browser=panel.findChild<SpikeArchiveBrowser *>();
        require(controls&&editor&&browser,"real panel constructs all three archive/rule widgets");click(&panel,"spikeArchiveTools");require(controls->isVisible(),"archive tools expand in the actual Spike panel");
        constexpr int lane=234; // a declared active channel in the deterministic Dummy population
        require(hub.start("127.0.0.1",port,port,"ctre",20000),"actual Dummy acquisition requested");
        require(until([&]{return hub.isConnected()&&panel.analysisStore()->snapshotLane(lane).events.size()>=4;}),"real hidden-capable detector receives Dummy spikes");
        require(hub.startRecording(temp.path(),"dummy-neural-source",4*1024*1024),"actual raw recording starts while detection continues");const QString recording=hub.recordingPath();
        const QString livePath=temp.filePath("live-events");named<QLineEdit>(controls,"analysisArchiveDestination")->setText(livePath);click(controls,"analysisStartArchive");
        require(until([&]{return panel.eventArchiveStatus().writtenEvents>100;}),"visible start button starts a real live archive");
        const auto hiddenBefore=panel.eventArchiveStatus().writtenEvents;panel.onDeactivated();panel.hide();
        require(until([&]{return panel.eventArchiveStatus().writtenEvents>=hiddenBefore+100;}),"immutable archive continues while Spike tab is hidden");
        panel.show();panel.onActivated();click(&panel,"spikePauseDisplay");const auto pausedBefore=panel.eventArchiveStatus().writtenEvents;
        require(until([&]{return panel.eventArchiveStatus().writtenEvents>=pausedBefore+100;}),"display pause does not pause event archiving");click(&panel,"spikePauseDisplay");
        click(controls,"analysisRuleEditor");named<QSpinBox>(editor,"ruleLane")->setValue(lane);click(editor,"ruleCapture");
        const auto training=panel.analysisStore()->snapshotLane(lane);require(!training.events.isEmpty(),"rule editor captures real applied-context training events");
        const quint64 trainLast=training.events.last().eventId;const qint64 trainFrame=training.events.last().sourceFrame;
        named<QSpinBox>(editor,"ruleUnit")->setValue(7);named<QDoubleSpinBox>(editor,"ruleTimeFrom")->setValue(-.1);named<QDoubleSpinBox>(editor,"ruleTimeTo")->setValue(.1);named<QDoubleSpinBox>(editor,"ruleVoltageFrom")->setValue(-1000);named<QDoubleSpinBox>(editor,"ruleVoltageTo")->setValue(1000);click(editor,"ruleAddBox");
        const auto definitions=editor->definitions();require(definitions.size()==1&&definitions[0].context.electrode==lane&&definitions[0].context.inputGain==60&&definitions[0].context.sourceSampleRate==20000&&definitions[0].context.preSamples==8,"draft binds exact actual detector context instead of guessed GUI provenance");
        const QString ruleFile=temp.filePath("held-out-rule.json");fileAction(named<QPushButton>(editor,"ruleSave"),ruleFile);auto saved=SpikeRuleSet::load(ruleFile,&error);require(saved&&saved->electrodes().size()==1,"actual rule Save produces independently readable persistent definitions");
        click(editor,"ruleApply");
        require(until([&]{const auto s=panel.analysisStore()->snapshotLane(lane);return std::any_of(s.events.cbegin(),s.events.cend(),[&](const auto &e){return e.eventId>trainLast&&e.sourceFrame>trainFrame&&e.classification==SpikeClassificationStatus::Assigned&&e.unitId==7&&e.ruleRevision>0;});}),"applied rule classifies later held-out stream events with a real revision");
        require(until([&]{return editor->property("appliedRevision").toULongLong()>0;}),"editor acknowledges the detector's applied revision");
        const quint64 assignedRevision=editor->property("appliedRevision").toULongLong();
        named<QSpinBox>(editor,"ruleUnit")->setValue(9);click(editor,"ruleAddBox");click(editor,"ruleApply");quint64 ambiguousRevision=0;
        require(until([&]{const auto s=panel.analysisStore()->snapshotLane(lane);for(const auto &e:s.events)if(e.ruleRevision>assignedRevision&&e.classification==SpikeClassificationStatus::Ambiguous&&e.unitId==-1){ambiguousRevision=e.ruleRevision;return true;}return false;}),"overlapping persistent candidates yield explicit ambiguity, never first-wins");
        require(until([&]{return panel.analysisStore()->snapshotLane(lane).totalDetected>45;}),"live acquisition exceeds selected-lane display retention");
        auto *gain=combo(&panel,"180×");gain->setCurrentText("180×");
        require(until([&]{return panel.eventArchiveStatus().state==SpikeArchiveState::Drained&&panel.eventArchiveStatus().finishVerified;}),"detector config change closes and verifies prior archive");
        SpikeArchiveStatus liveStatus;const auto live=readAll(livePath,&liveStatus);verifyRecords(livePath,live,liveStatus);
        require(liveStatus.sourceIntegrity["end_reason"].toString()=="analysis_interrupted_or_reconfigured","config-change archive explicitly records interrupted/reconfigured boundary");
        int laneArchived=0;bool oldUnchanged=true,assigned=false,ambiguous=false;
        for(const auto &r:live)if(r.event.electrode==lane){++laneArchived;if(r.event.eventId<=trainLast)oldUnchanged=oldUnchanged&&r.event.classification==SpikeClassificationStatus::Unassigned&&r.event.unitId==-1;if(r.event.classification==SpikeClassificationStatus::Assigned)assigned=true;if(r.event.classification==SpikeClassificationStatus::Ambiguous)ambiguous=true;}
        require(laneArchived>20&&oldUnchanged&&assigned&&ambiguous,"archive exceeds 20-row display capacity and preserves old labels plus later assigned/ambiguous events");
        const QString incompatiblePath=temp.filePath("incompatible-events");controls->setArchiveDestination(incompatiblePath);click(controls,"analysisStartArchive");
        const auto incompatibleBefore=panel.analysisStore()->snapshotLane(lane);const quint64 incompatibleBoundary=incompatibleBefore.events.isEmpty()?0:incompatibleBefore.events.last().eventId;
        require(until([&]{const auto s=panel.analysisStore()->snapshotLane(lane);return std::any_of(s.events.cbegin(),s.events.cend(),[&](const auto &e){return e.eventId>incompatibleBoundary&&e.classification==SpikeClassificationStatus::Incompatible&&e.unitId==-1;});}),"changed gain explicitly rejects incompatible saved rules for future archived events");
        require(until([&]{return panel.eventArchiveStatus().writtenEvents>100;}),"new detector run archives independently");
        const auto beforeArchiveStop=panel.analysisStore()->snapshotLane(lane).observedSamples;click(controls,"analysisStopArchive");
        require(hub.isRunning()&&panel.analysisRunning(),"archive Stop button leaves real acquisition and detection running");
        require(until([&]{return panel.eventArchiveStatus().finishVerified&&panel.eventArchiveStatus().state==SpikeArchiveState::Drained;}),"asynchronous archive stop verifies its drained terminal before reopen");
        require(until([&]{return panel.analysisStore()->snapshotLane(lane).observedSamples>beforeArchiveStop+512;}),"detector keeps processing while stopped archive finalizes");
        SpikeArchiveStatus manualStopStatus;const auto manuallyStopped=readAll(incompatiblePath,&manualStopStatus);verifyRecords(incompatiblePath,manuallyStopped,manualStopStatus);
        require(manualStopStatus.sourceIntegrity["end_reason"].toString()=="user_stopped_archive_at_emission_boundary","manual archive stop records its actual emission boundary");
        require(std::any_of(manuallyStopped.cbegin(),manuallyStopped.cend(),[&](const auto &r){return r.event.electrode==lane&&r.event.classification==SpikeClassificationStatus::Incompatible&&r.event.unitId==-1;}),"independent archive readback preserves explicit incompatibility with no candidate label");
        const QString normalStopPath=temp.filePath("normal-stop-events");controls->setArchiveDestination(normalStopPath);
        require(until([&]{return named<QPushButton>(controls,"analysisStartArchive")->isEnabled();}),"completed asynchronous writer permits a new archive");click(controls,"analysisStartArchive");
        require(until([&]{return panel.eventArchiveStatus().writtenEvents>100;}),"new archive receives events before source stop");
        require(until([&]{return hub.recordedBytes()>=64000LL*kFrameBytes;},10000),"actual raw fixture contains at least 64000 recorded frames before stop");
        const auto stopBefore=panel.analysisStore()->snapshotLane(lane);hub.stop();const auto stopAfter=panel.analysisStore()->snapshotLane(lane);
        require(!panel.analysisRunning()&&stopAfter.observedSamples>=stopBefore.observedSamples&&stopAfter.quality.pendingWindowEvents==0&&!stopAfter.quality.stoppedEarly,"normal source stop drains detector input and pending tail bookkeeping");
        require(until([&]{return panel.eventArchiveStatus().finishVerified;}),"source-stop writer finishes before independent reopen");
        SpikeArchiveStatus incompatibleStatus;const auto incompatible=readAll(normalStopPath,&incompatibleStatus);verifyRecords(normalStopPath,incompatible,incompatibleStatus);
        require(incompatibleStatus.state==SpikeArchiveState::Drained&&incompatibleStatus.sourceIntegrity["producer_detector_drained"].toBool()&&incompatibleStatus.runId!=liveStatus.runId,"source stop records drained lifecycle and independent new-run identity");
        SessionManifestData recorded;require(SessionManifest::read(recording,&recorded,&error),"actual Dummy recording manifest reopens: "+error);
        std::cout<<"MEASURE recording frames="<<recorded.totalFrames<<" parts="<<recorded.parts.size()<<" complete="<<recorded.complete<<" ingressDrops="<<recorded.ingressDroppedFrames<<" recordingDrops="<<recorded.recordingDroppedFrames<<" gaps="<<recorded.integrity.upstreamMissingFrames<<" repeated="<<recorded.integrity.repeatedFrames<<" irregular="<<recorded.integrity.irregularJumps<<" mismatched="<<recorded.integrity.intraFrameMismatchFrames<<std::endl;
        require(recorded.totalFrames>=64000&&recorded.parts.size()>1,"actual Dummy recording contains all bounded fixture frames and multiple raw parts");
        require(recorded.complete,"actual Dummy recording closes with a complete source manifest");const auto originalHashes=hashFiles(recording);
        // Open the real archive browser through its ordinary directory chooser.
        controls->setArchiveDestination(livePath);fileAction(named<QPushButton>(controls,"analysisOpenArchive"),livePath);
        require(until([&]{return !browser->busy()&&browser->visibleEventCount()>0;}),"visible archive-open action loads a bounded persistent page");
        const quint64 selected=browser->selectedEventId();named<QSpinBox>(browser,"archiveLabelUnit")->setValue(11);named<QLineEdit>(browser,"archiveLabelNote")->setText("Dummy integration annotation");click(browser,"archiveAppendLabel");
        require(until([&]{return !browser->busy();}),"archive annotation async action finishes");
        SpikeEventArchiveReader labelled;require(labelled.open(livePath,&error),"annotation archive independently reopened");const auto labels=labelled.readAnnotations(selected);
        require(!labels.annotations.isEmpty()&&labels.annotations.last().unitId==11&&labels.annotations.last().runId==liveStatus.runId,"browser manual label persists by run/event identity");
        const auto liveAfterLabel=readAll(livePath);bool immutable=liveAfterLabel.size()==live.size();for(int i=0;immutable&&i<live.size();++i)immutable=liveAfterLabel[i].event.eventId==live[i].event.eventId&&liveAfterLabel[i].event.classification==live[i].event.classification&&liveAfterLabel[i].event.unitId==live[i].event.unitId&&liveAfterLabel[i].waveform==live[i].waveform;
        require(immutable,"append-only manual label leaves automated assignments and waveform bytes immutable");
        for(const QString &name:{QString("dark"),QString("light")}) {theme.apply(name);panel.setWaveTheme(colors(theme.currentPalette()));screenshot(controls,"analysis-controls-"+name,theme.currentPalette().appBg);screenshot(editor,"rule-editor-"+name,theme.currentPalette().appBg);screenshot(browser,"archive-browser-"+name,theme.currentPalette().appBg);}
        editor->hide();browser->hide();
        // Same recorded bytes, fixed current configuration and persistent rule:
        // replay from the beginning, close a seek boundary, then archive to EOF.
        gain->setCurrentText("60×");require(hub.startReplay(recording,20000),"real recorded session reopens for replay");
        click(controls,"analysisRuleEditor");named<QSpinBox>(editor,"ruleLane")->setValue(lane);
        const qint64 pausedSourceOrigin=hub.currentFrameIndex();
        require(until([&]{return editor->property("appliedRevision").toULongLong()==ambiguousRevision;})&&panel.analysisStore()->snapshotLane(lane).observedSamples==0,"paused replay exposes its previous rule applied at worker initialization without consuming source samples");
        fileAction(named<QPushButton>(editor,"ruleLoad"),ruleFile);click(editor,"ruleApply");editor->close();click(controls,"analysisRuleEditor");
        require(hub.currentFrameIndex()==pausedSourceOrigin&&panel.analysisStore()->snapshotLane(lane).observedSamples==0&&editor->property("appliedRevision").toULongLong()==ambiguousRevision&&named<QLabel>(editor,"ruleRevision")->text().contains(QStringLiteral("版本：%1").arg(ambiguousRevision)),"close/reopen before new samples shows actual prior revision rather than requested pending rule");editor->hide();
        const QString seekPath=temp.filePath("before-seek");controls->setArchiveDestination(seekPath);click(controls,"analysisStartArchive");hub.replayTogglePlay();
        require(until([&]{return panel.eventArchiveStatus().writtenEvents>100;}),"replay archive receives actual recorded events");hub.replayPause();pump(50);const auto seekEpoch=hub.timelineEpoch();hub.replaySeekFraction(.5);
        require(until([&]{return hub.timelineEpoch()!=seekEpoch&&panel.eventArchiveStatus().state==SpikeArchiveState::Drained&&panel.eventArchiveStatus().finishVerified;}),"replay seek closes and finalizes prior archive before independent reopen");
        SpikeArchiveStatus seekStatus;readAll(seekPath,&seekStatus);require(seekStatus.finishVerified&&seekStatus.sourceIntegrity["end_reason"].toString()=="timeline_changed"&&seekStatus.sourceIntegrity["stopped_early"].toBool(),"seek archive retains explicit qualified old-timeline boundary");
        hub.replayJumpStart();const QString replayPath=temp.filePath("replay-whole-events");controls->setArchiveDestination(replayPath);click(controls,"analysisStartArchive");hub.replayTogglePlay();
        require(until([&]{return panel.eventArchiveStatus().state==SpikeArchiveState::Completed&&!panel.analysisRunning();},20000),"EOF drains full recorded replay into a completed archive");
        SpikeArchiveStatus replayStatus;const auto replay=readAll(replayPath,&replayStatus);verifyRecords(replayPath,replay,replayStatus);
        require(replayStatus.sourceIntegrity["end_reason"].toString()=="source_eof"&&replayStatus.sourceIntegrity["producer_detector_drained"].toBool()&&replayStatus.sourceIntegrity["pending_window_events"].toString()=="0","EOF archive explicitly records drained producer and zero pending windows");
        const qint64 replayPosition=hub.currentFrameIndex();
        const QString offlinePath=temp.filePath("offline-whole-events");click(controls,"analysisOfflineToggle");controls->setOfflinePaths(recording,offlinePath);click(controls,"analysisRunOffline");
        require(until([&]{return panel.offlineJob()&&!panel.offlineJob()->isRunning();},25000),"actual offline button completes dedicated whole-file analysis");auto offlineStatus=panel.offlineJob()->status();
        require(offlineStatus.state==SpikeArchiveState::Completed&&offlineStatus.inputEof&&offlineStatus.processedFrames==recorded.totalFrames&&offlineStatus.readFrames==recorded.totalFrames,"offline analysis processes every recorded frame losslessly");
        require(hub.currentFrameIndex()==replayPosition&&!panel.analysisRunning(),"offline analysis does not move shared replay or restart acquisition");
        SpikeArchiveStatus offlineArchive;const auto offline=readAll(offlinePath,&offlineArchive);verifyRecords(offlinePath,offline,offlineArchive);
        require(offlineStatus.detectedEvents==qint64(offline.size())&&offlineStatus.archive.writtenEvents==quint64(offline.size())&&offlineStatus.detectedEvents>offlineStatus.retainedEvents,"offline persistent event count exceeds its bounded display retention without loss");
        QMap<QPair<int,qint64>,SpikeArchiveRecord> bySource;for(const auto &r:replay)bySource.insert({r.event.electrode,r.event.sourceFrame},r);
        bool equal=bySource.size()==replay.size()&&offline.size()==replay.size();
        for(const auto &r:offline){auto it=bySource.constFind({r.event.electrode,r.event.sourceFrame});equal=equal&&it!=bySource.cend();if(it!=bySource.cend())equal=equal&&r.waveform==it->waveform&&r.event.preSamples==it->event.preSamples&&r.event.inputGain==it->event.inputGain&&r.event.unitId==it->event.unitId&&r.event.classification==it->event.classification&&r.event.ruleRevision==it->event.ruleRevision;}
        require(equal,"independent offline/replay archives match every source event, waveform float and automated label");
        const QString cancelPath=temp.filePath("offline-cancelled");controls->setOfflinePaths(recording,cancelPath);click(controls,"analysisRunOffline");
        require(until([&]{return panel.offlineJob()->isRunning()&&panel.offlineJob()->status().archive.writtenEvents>0;},8000),"cancellable offline job has accepted a nonempty prefix");click(controls,"analysisCancelOffline");
        require(until([&]{return !panel.offlineJob()->isRunning();},15000),"actual cancellation button joins offline job");const auto cancelled=panel.offlineJob()->status();SpikeArchiveStatus cancelArchive;const auto prefix=readAll(cancelPath,&cancelArchive);verifyRecords(cancelPath,prefix,cancelArchive);
        require(cancelled.state==SpikeArchiveState::Cancelled&&cancelArchive.state==SpikeArchiveState::Cancelled&&cancelArchive.writtenEvents>0&&cancelArchive.writtenEvents==cancelArchive.acceptedEvents&&cancelled.processedFrames<recorded.totalFrames,"cancelled job preserves every accepted event with a recoverable explicit terminal");
        require(hashFiles(recording)==originalHashes,"archive, replay, annotation and offline jobs leave all raw recording files and manifest unchanged");
        hub.stop();panel.shutdown();panel.close();pump(30);std::cout<<"dummy_neural_archive_e2e ok checks="<<checks<<std::endl;return 0;
    }catch(const std::exception &e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<std::endl;return 1;}
}
