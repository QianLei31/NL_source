#include "io/spike_event_exporter.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ccv2::SpikeSnippetStore store;
    if (!store.configure(1, 4, 2)) return 1;
    store.resetTimeline(8);
    ccv2::SpikeEvent e;
    e.epoch=8; e.adcChannel=0; e.electrode=0; e.sourceSampleRate=20000; e.inputGain=60; e.preSamples=1;
    float wave[] = {0, -0.0001f, -0.0003f, 0.0001f};
    for(int i=0;i<3;++i) { e.sourceFrame=100+i*100; if(!store.addEventIfEpoch(0,wave,e)) return 2; }
    store.addCoverageIfEpoch(0,1000,0,999,8);
    auto snapshot=store.snapshotLane(0);
    store.setCandidateUnit(8,0,{snapshot.events[1].sequence},2);
    snapshot=store.snapshotLane(0);
    ccv2::SpikeDetectConfig cfg; cfg.inputGain=60; cfg.preSamples=1; cfg.postSamples=2;
    const auto metadata=ccv2::spikeAnalysisMetadata(cfg,2,{{0,75}});
    QString error;
    const QString path=dir.filePath("spikes.json");
    if(!ccv2::exportSpikeLaneJson(path,snapshot,metadata,&error)) {std::cerr<<error.toStdString();return 3;}
    QFile file(path); if(!file.open(QIODevice::ReadOnly))return 4;
    const QByteArray original=file.readAll(); file.close();
    const auto root=QJsonDocument::fromJson(original).object();
    const auto events=root["events"].toArray();
    if(root["schema_version"].toInt()!=1 || root["total_detected"].toString()!="3" || root["evicted_or_not_retained_events"].toString()!="1" || events.size()!=2)return 5;
    if(events[1].toObject()["candidate_unit_id"].toInt()!=2 || events[1].toObject()["source_frame"].toString()!="300")return 6;
    if(root["analysis"].toObject()["input_gain"].toDouble()!=60 || root["analysis"].toObject()["reference_mode"].toInt()!=2)return 7;
    if(events[0].toObject()["waveform_input_volts"].toArray().size()!=4)return 8;
    snapshot.waveforms.clear();
    if(ccv2::exportSpikeLaneJson(path,snapshot,metadata,&error))return 9;
    if(!file.open(QIODevice::ReadOnly) || file.readAll()!=original)return 10;
    std::cout<<"spike_event_export_smoke OK\n"; return 0;
}
