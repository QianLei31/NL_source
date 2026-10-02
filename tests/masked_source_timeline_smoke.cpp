#include "service/session_hub.h"
#include "network/replay_controller.h"
#include "io/session_manifest.h"
#include "core/constants.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <functional>
#include <iostream>

bool waitFor(const std::function<bool()> &predicate) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if(predicate())return true;
        QThread::msleep(2);
    }
    return predicate();
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); QTemporaryDir tmp;
    QTcpServer server; if(!server.listen(QHostAddress::LocalHost,0))return 1;
    QByteArray bytes(128*ccv2::kFrameBytes,'\0');
    for(int f=0;f<128;++f) for(int ch=0;ch<256;++ch) {
        const int timestamp=(f==20 && ch==0)?1000:f;
        qToLittleEndian<quint32>((quint32(timestamp)<<12)|2048u,bytes.data()+f*ccv2::kFrameBytes+ch*4);
    }
    QObject::connect(&server,&QTcpServer::newConnection,&app,[&] {
        auto *socket=server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
            if(socket->readAll().startsWith("ctre"))socket->write(bytes);
        });
    });
    ccv2::SessionHub hub;
    auto live=std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>();
    hub.addSubscriber(live);
    if(!hub.start("127.0.0.1",server.serverPort(),server.serverPort(),"ctre",20000))return 2;
    if(!hub.startRecording(tmp.path(),"masked"))return 3;
    QString folder=hub.recordingPath();
    if(!waitFor([&]{return hub.statistics().distributedFrames>=128;}))return 4;
    QVector<qint64> liveFrames; QVector<bool> liveValidity;
    QByteArray part; ccv2::StreamBlockInfo info;
    while(live->pop(part,0,nullptr,&info)) {liveFrames+=info.frameIndices;liveValidity+=info.frameValid;}
    hub.stopRecording(); hub.stop();
    if(liveFrames.size()!=128 || liveValidity.size()!=128 || liveValidity[20])return 5;
    for(int f=0;f<128;++f)if(liveFrames[f]!=f)return 6;
    ccv2::SessionManifestData manifest; QString error;
    if(!ccv2::SessionManifest::read(folder,&manifest,&error))return 7;
    if(!manifest.frameValidityKnown || manifest.invalidFrameRanges.size()!=1 || manifest.invalidFrameRanges[0].startFrame!=20)return 8;
    auto replayQueue=std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>();
    ccv2::ReplayController replay(replayQueue);
    if(!replay.open(folder,20000))return 9;
    replay.play(); if(!waitFor([&]{return replay.currentFrame()==128;}))return 10;
    QVector<qint64> replayFrames; QVector<bool> replayValidity;
    while(replayQueue->pop(part,0,nullptr,&info)) {replayFrames+=info.frameIndices;replayValidity+=info.frameValid;}
    if(liveFrames!=replayFrames || liveValidity!=replayValidity)return 11;
    const auto provenance=replay.sourceProvenance();
    if(!provenance["has_manifest"].toBool() || !provenance["frame_validity_known"].toBool() || provenance["integrity_complete"].toBool())return 12;
    // Raw BIN has no validity mask. Reject internally inconsistent timestamps
    // before reconciliation without inventing verified-source provenance.
    const QString rawPath=tmp.filePath("corrupted_raw.bin");
    QFile rawFile(rawPath); if(!rawFile.open(QIODevice::WriteOnly)||rawFile.write(bytes)!=bytes.size())return 13;
    rawFile.close();
    ccv2::ReplayController raw(replayQueue);
    if(!raw.open(rawPath,20000)||raw.frameValidityKnown())return 14;
    QVector<qint64> rawFrames;
    while(true){if(!raw.readNextBlock(7,&part,&info))return 15;if(part.isEmpty())break;
        if(!info.frameValid.isEmpty())return 16;rawFrames+=info.frameIndices;}
    if(rawFrames!=liveFrames||raw.sourceProvenance()["frame_validity_known"].toBool())return 17;
    raw.seekFrame(67); if(!waitFor([&]{return !raw.isSeeking()&&raw.currentFrame()==67;}))return 18;
    if(raw.nextSourceFrame()!=67||!raw.readNextBlock(20,&part,&info)||info.firstFrame()!=67)return 19;
    for(int i=0;i<info.frameIndices.size();++i)
        if(info.frameIndices[i]!=67+i||info.frameIndices[i]%4!=(67+i)%4)return 20;
    raw.seekFrame(0);if(!waitFor([&]{return !raw.isSeeking()&&raw.currentFrame()==0;}))return 21;
    raw.play();if(!waitFor([&]{return raw.currentFrame()==128;}))return 22;
    rawFrames.clear();while(replayQueue->pop(part,0,nullptr,&info))rawFrames+=info.frameIndices;
    if(rawFrames!=liveFrames)return 23;
    std::cout<<"masked_source_timeline_smoke OK\n";return 0;
}
