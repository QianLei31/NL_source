// Functional, measured integration coverage using the application's real Dummy,
// TCP receiver, SessionHub, raw recorder, replay and export implementations.
// SPI evidence is simulated wire-level evidence, NEVER physical chip validation.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "config/config_manager.h"
#include "core/channel_address_state.h"
#include "core/channel_config_model.h"
#include "core/channel_routing.h"
#include "core/constants.h"
#include "core/network_state.h"
#include "core/tdm_context.h"
#include "io/matlab_channel_exporter.h"
#include "io/session_manifest.h"
#include "network/timestamp_checker.h"
#include "network/replay_controller.h"
#include "service/dummy_stream_server.h"
#include "service/session_hub.h"
#include "ui/spi_control_panel.h"

using namespace ccv2;
namespace {
void require(bool value, const QString &why) {
    if (!value) throw std::runtime_error(why.toStdString());
}
bool waitFor(const std::function<bool()> &predicate, int ms = 5000) {
    QElapsedTimer timer; timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < ms);
    return predicate();
}
void spin(int ms) { waitFor([] { return false; }, ms); }
int port() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "allocate loopback port");
    return server.serverPort();
}
QByteArray readFile(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "open " + path);
    return file.readAll();
}
void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly), "write " + path);
    require(file.write(bytes) == bytes.size(), "write full " + path);
}
void pass(const QString &id, const QString &evidence) {
    std::cout << "PASS " << id.toStdString() << " | " << evidence.toStdString() << std::endl;
}
void drain(const std::shared_ptr<ThreadSafeQueue<QByteArray>> &queue, QByteArray *out) {
    QByteArray chunk; while (queue->pop(chunk, 0)) out->append(chunk);
}
quint32 wordAt(const QByteArray &data, qint64 frame, int channel) {
    return qFromLittleEndian<quint32>(data.constData() + frame * kFrameBytes + channel * 4);
}
QPushButton *button(QWidget &widget, const QString &text) {
    for (auto *candidate : widget.findChildren<QPushButton *>())
        if (candidate->text() == text) return candidate;
    throw std::runtime_error(("missing button " + text).toStdString());
}
QString wire(const QString &bits) {
    bool ok = false; quint32 value = bits.toUInt(&ok, 2);
    require(ok && bits.size() == 32, "test command bits valid");
    return QStringLiteral("spi%1").arg(value, 8, 16, QLatin1Char('0'));
}

// A bounded protocol observation peer: records exact application TX payloads.
// It does not model or assert hardware register/stimulation state.
struct ControlPeer {
    QTcpServer server;
    QStringList messages;
    bool shortReply = false;
    ControlPeer() {
        require(server.listen(QHostAddress::LocalHost, 0), "control observer listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                auto pending = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, pending] {
                    pending->append(socket->readAll());
                    if (pending->size() < 11) return;
                    messages.append(QString::fromLatin1(*pending));
                    pending->clear();
                    socket->write(QByteArray(shortReply ? 3 : 12, '\0'));
                    if (shortReply) socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
};

void controlCases(const QString &root) {
    ControlPeer peer;
    ConfigManager cfg(QDir(root).filePath("control.ini"));
    ConfigMap values = cfg.load();
    auto &stim = values["Stimulator"];
    stim["block"] = "00110000"; stim["addr_channel"] = "01";
    stim["amplitude"] = "101010101"; stim["polarity"] = "10";
    stim["dac_channel"] = "11"; stim["compensate"] = "1"; stim["step"] = "0";
    require(cfg.save(values), "seed control configuration");
    NetworkState endpoint("127.0.0.1", peer.server.serverPort(), peer.server.serverPort());
    ChannelAddressState channels;
    SpiControlPanel panel(&cfg, &endpoint, &channels);
    bool busy = false; int applied = 0; int degraded = 0; int unreachable = 0;
    QObject::connect(&panel, &SpiControlPanel::busyChanged, &panel, [&](bool value) { busy = value; });
    QObject::connect(&panel, &SpiControlPanel::spiCommandApplied, &panel, [&](const QString &) { ++applied; });
    QObject::connect(&panel, &SpiControlPanel::controlLinkResult, &panel,
                     [&](bool reachable, bool badReply, const QString &) {
        if (badReply) ++degraded; if (!reachable) ++unreachable;
    });
    auto runButton = [&](const QString &label, const QStringList &expected) {
        int start = peer.messages.size(); int oldApplied = applied;
        button(panel, label)->click();
        require(waitFor([&] { return !busy && peer.messages.size() >= start + expected.size(); }, 15000),
                "control button completion " + label);
        require(peer.messages.mid(start) == expected, "exact TX sequence " + label);
        require(applied - oldApplied == expected.size(), "applied signal count " + label);
    };
    const QList<QPair<QString, QString>> globals = {
        {"模拟RST", "spi1c000000"}, {"模拟解除RST", "spi20000000"},
        {"DAC开", "spi24000000"}, {"DAC关", "spi34000000"},
        {"CBOK低", "spi48000000"}, {"Dummy", "spi00000000"}};
    for (const auto &g : globals) runButton(g.first, {g.second});
    pass("C01", "six global command buttons: exact TCP bytes and applied signals");
    QLineEdit *direct = nullptr;
    for (auto *edit : panel.findChildren<QLineEdit *>())
        if (edit->placeholderText().startsWith(QStringLiteral("例如"))) direct = edit;
    require(direct, "direct command field");
    direct->setText("00010000_00000101 0000000001100000");
    runButton("发送", {"spi10050060"});
    require(cfg.load()["Spi"]["direct_command"] == direct->text(), "direct command persisted");
    for (const QString &invalid : {QString("101"), QString(32, '2')}) {
        int before = peer.messages.size(); direct->setText(invalid); button(panel, "发送")->click(); spin(30);
        require(peer.messages.size() == before && !busy, "invalid command rejected before network");
    }
    pass("C02", "direct normalization/persistence and invalid length/alphabet rejected");
    for (const auto &gain : QList<QPair<QString, quint16>>{{"增益全高", 0x005e}, {"增益全低", 0x007e}}) {
        QStringList expected;
        for (int block = 0; block < 64; ++block) for (int ch = 0; ch < 4; ++ch) {
            expected << wire(ChannelConfigModel::buildRecCommand(block, ch, gain.second)) << "spi00000000";
        }
        runButton(gain.first, expected);
    }
    pass("C03", "gain high/low: each 256 REC writes plus 256 zero spacer commands, ordered");
    const QString stimPayload = "0001100011000001";
    runButton("输出", {"spi00000000", wire(stimPayload + "0" + "11" + "0" + "1" + "10" + "101010101")});
    runButton("关闭", {"spi00000000", wire(stimPayload + "1" + "11" + "0" + "1" + "10" + "101010101")});
    require(cfg.load()["Stimulator"]["dac_channel"] == "11" && cfg.load()["Stimulator"]["amplitude"] == "101010101",
            "stimulation config persisted");
    pass("C04", "stimulation output/off: configured independent outer/DAC address and all payload bits, zero preamble");
    QStringList batch = {ChannelConfigModel::buildRecCommand(7, 2, 0x05e),
                         ChannelConfigModel::buildDacCommand(7, 2, 0x6123),
                         ChannelConfigModel::buildCtCommand(7, 2, 0xd2a5)};
    int start = peer.messages.size(); int oldApplied = applied;
    panel.enqueueExternalCommands("E2E REC/DAC/CT", batch, 100);
    require(waitFor([&] { return !busy && peer.messages.size() >= start + 6; }), "batch completed");
    QStringList expected; for (const auto &cmd : batch) expected << wire(cmd) << "spi00000000";
    require(peer.messages.mid(start) == expected && applied - oldApplied == 6, "batch exact six TX and signals");
    pass("C05", "external REC/DAC/CT queue: exact ordered payload and mandatory trailing spacers");
    peer.shortReply = true; int oldDegraded = degraded; oldApplied = applied;
    direct->setText("00010000000000010000000001100000"); button(panel, "发送")->click();
    require(waitFor([&] { return !busy && degraded > oldDegraded; }), "short reply signals degraded");
    require(applied == oldApplied + 1, "sent-but-unconfirmed retained in shadow event");
    peer.shortReply = false;
    const int goodPort = peer.server.serverPort(); endpoint.setEndpoint("127.0.0.1", port(), goodPort);
    oldApplied = applied; const int oldUnreachable = unreachable;
    button(panel, "发送")->click();
    require(waitFor([&] { return !busy && unreachable > oldUnreachable; }, 6000), "unreachable control reported");
    require(applied == oldApplied, "failed connection must not apply shadow command");
    endpoint.setEndpoint("127.0.0.1", goodPort, goodPort);
    runButton("发送", {"spi10010060"});
    pass("C06", "short reply -> degraded + sent shadow; refused connection -> no shadow; recovery succeeds");
    start = peer.messages.size();
    QStringList longBatch; for (int i = 0; i < 20; ++i) longBatch << batch.first();
    panel.enqueueExternalCommands("endpoint cancellation", longBatch, 30000);
    require(waitFor([&] { return peer.messages.size() > start; }), "queued batch started");
    endpoint.setEndpoint("127.0.0.1", port(), goodPort);
    require(waitFor([&] { return !busy; }), "old-endpoint batch cancelled");
    require(peer.messages.size() - start < 40, "remaining old-endpoint commands withheld");
    endpoint.setEndpoint("127.0.0.1", goodPort, goodPort);
    runButton("DAC关", {"spi34000000"});
    button(panel, "清空")->click();
    bool cleared = false; for (auto *console : panel.findChildren<QPlainTextEdit *>())
        if (console->maximumBlockCount() == 400) cleared = console->toPlainText().isEmpty();
    require(cleared, "console clear");
    panel.flushPendingConfig();
    pass("C07", "endpoint change cancels pending commands; new endpoint works; console clears");
}


void simulatedControlState(const QString &root) {
    const int endpoint = port(); DummyStreamServer dummy; QString error;
    require(dummy.start(endpoint, endpoint, &error), "state-model Dummy listen");
    ConfigManager cfg(QDir(root).filePath("simulated_control.ini"));
    ConfigMap configuration = cfg.load();
    auto &stim = configuration["Stimulator"];
    stim["block"] = "00110000"; stim["addr_channel"] = "01";
    stim["amplitude"] = "101010101"; stim["polarity"] = "10";
    stim["dac_channel"] = "11"; stim["compensate"] = "1"; stim["step"] = "0";
    require(cfg.save(configuration), "state-model stimulation config");
    NetworkState network("127.0.0.1", endpoint, endpoint); ChannelAddressState selection;
    SpiControlPanel panel(&cfg, &network, &selection); bool busy = false;
    QObject::connect(&panel, &SpiControlPanel::busyChanged, &panel, [&](bool value) { busy = value; });
    auto click = [&](const QString &label, quint64 commands) {
        const quint64 before = dummy.protocolState().spiCommands;
        button(panel, label)->click();
        require(waitFor([&] { return !busy && dummy.protocolState().spiCommands == before + commands; }, 15000),
                "SPI panel -> actual Dummy completion: " + label);
    };
    click("增益全高", 512);
    for (int channel = 0; channel < 256; ++channel)
        require(dummy.protocolState().rec[channel] == 0x005e && dummy.channelGain(channel) == 180.0,
                "all channels simulated high gain");
    click("增益全低", 512);
    for (int channel = 0; channel < 256; ++channel)
        require(dummy.protocolState().rec[channel] == 0x007e && dummy.channelGain(channel) == 60.0,
                "all channels simulated low gain");
    click("模拟RST", 1); require(dummy.protocolState().analogResetAsserted == true, "simulated reset asserted");
    click("模拟解除RST", 1); require(dummy.protocolState().analogResetAsserted == false, "simulated reset released");
    click("DAC开", 1); require(dummy.protocolState().globalDacEnabled == true, "simulated global DAC enabled");
    click("DAC关", 1); require(dummy.protocolState().globalDacEnabled == false, "simulated global DAC disabled");
    click("CBOK低", 1); require(dummy.protocolState().cbokLow == true, "simulated CBOK low");
    click("输出", 2);
    require(dummy.protocolState().dac[48 * 4 + 1] == 0x6d55, "simulated configured stimulation enabled state");
    click("关闭", 2);
    require(dummy.protocolState().dac[48 * 4 + 1] == 0xed55, "simulated configured stimulation disabled state");
    QStringList batch = {ChannelConfigModel::buildRecCommand(7, 2, 0x047e),
                         ChannelConfigModel::buildDacCommand(7, 2, 0x6123),
                         ChannelConfigModel::buildCtCommand(7, 2, 0xd2a5)};
    const quint64 before = dummy.protocolState().spiCommands;
    panel.enqueueExternalCommands("actual Dummy state", batch, 100);
    require(waitFor([&] { return !busy && dummy.protocolState().spiCommands == before + 6; }), "state writes complete");
    auto state = dummy.protocolState(); const int channel = 7 * 4 + 2;
    require(state.rec[channel] == 0x047e && state.dac[channel] == 0x6123 && state.dacCt[channel] == 0xd2a5 &&
            !dummy.channelSignalEnabled(channel), "REC/DAC/CT simulated state updated through actual GUI send pipeline");
    require(state.rec[channel + 1] == 0x007e && state.dac[channel + 1] == 0x8000 && state.dacCt[channel + 1] == 0,
            "unaddressed simulated state unchanged");
    require(state.unsupportedCommands == 0 && state.malformedCommands == 0, "all GUI protocol payloads accepted");
    panel.flushPendingConfig();
    pass("C08", QString("actual SPI panel -> built-in Dummy: %1 commands; all256 gain states; global flags; selected REC/DAC/CT state")
         .arg(state.spiCommands));
}

struct Capture { QString path; SessionManifestData manifest; QByteArray bytes; };
Capture readCapture(const QString &path) {
    Capture capture; capture.path = path; QString error;
    require(SessionManifest::read(path, &capture.manifest, &error), "read manifest: " + error);
    for (const auto &part : capture.manifest.parts) {
        QByteArray bytes = readFile(QDir(path).filePath(part.fileName));
        require(bytes.size() == part.bytes && part.bytes == part.frames * kFrameBytes, "part sizes match manifest");
        capture.bytes += bytes;
    }
    require(capture.bytes.size() == capture.manifest.totalBytes &&
            capture.manifest.totalFrames * kFrameBytes == capture.bytes.size(), "total raw counts match manifest");
    return capture;
}

Capture liveCapture(const QString &root, bool dualPort) {
    const int control = port(); int data = dualPort ? port() : control;
    while (dualPort && data == control) data = port();
    DummyStreamServer dummy; dummy.setTdmSimulation(true); QString error;
    require(dummy.start(control, data, &error), "Dummy listen: " + error);
    SessionHub hub; auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(512);
    hub.addSubscriber(queue); QByteArray received;
    auto stalledDisplay = std::make_shared<ThreadSafeQueue<QByteArray>>(1);
    hub.addSubscriber(stalledDisplay);
    hub.tdmContext()->setState(true, false);
    hub.setAnalysisMetadata(QJsonObject{{"threshold_mode", "rms"}, {"gain", 60}});
    bool controlReachable = false;
    QObject::connect(&hub, &SessionHub::controlLinkResult, &hub,
                     [&](bool reachable, const QString &) { controlReachable |= reachable; });
    require(hub.start("127.0.0.1", control, data), "live starts");
    require(waitFor([&] { drain(queue, &received); return hub.state() == SessionHub::State::Live && received.size() >= 320 * kFrameBytes; }),
            "live receives complete Dummy frames");
    require(!hub.start("127.0.0.1", control, data), "duplicate live start refused");
    require(controlReachable && hub.isConnected(), "control and data link connected");
    const QString name = dualPort ? "live_dual" : "live_single";
    require(hub.startRecording(root, name, 64LL * kFrameBytes), "start raw recording");
    const QString path = hub.recordingPath();
    require(hub.startRecording(root, "must_not_replace", 0) && hub.recordingPath() == path,
            "repeated record start idempotent");
    require(waitFor([&] { drain(queue, &received); return hub.recordedBytes() >= 1280LL * kFrameBytes; }), "raw frames recorded");
    hub.setAnalysisMetadata(QJsonObject{{"threshold_mode", "absolute"}, {"gain", 180}});
    hub.stopRecording();
    drain(queue, &received);
    Capture capture = readCapture(path);
    require(capture.manifest.complete && !capture.manifest.active && capture.manifest.integrity.clean(), "clean finalized manifest");
    require(hub.statistics().subscriberDroppedFrames > 0 && capture.manifest.recordingDroppedFrames == 0,
            "stalled display loses frames without corrupting recording");
    require(capture.manifest.parts.size() > 1 && capture.manifest.frameValidityKnown &&
            capture.manifest.invalidFrameRanges.isEmpty(), "split recording validity known/clean");
    for (const auto &part : capture.manifest.parts) require(part.bytes <= 64LL * kFrameBytes, "split byte limit observed");
    require(received.indexOf(capture.bytes) >= 0, "recorded bytes are exact contiguous live subscriber payload");
    require(capture.manifest.metadata.frameOrigin == (wordAt(capture.bytes, 0, 0) >> kTimestampShift),
            "recorded frame origin equals independently observed Dummy hardware timestamp");
    require(capture.manifest.metadata.source == "live" && capture.manifest.metadata.host == "127.0.0.1" &&
            capture.manifest.metadata.controlPort == control && capture.manifest.metadata.dataPort == data &&
            capture.manifest.metadata.sampleRate == 20000.0 && capture.manifest.metadata.tdmKnown &&
            capture.manifest.metadata.tdmEnabled && !capture.manifest.metadata.tdmEvenFirst,
            "recorded live endpoint/rate/TDM metadata");
    require(capture.manifest.metadata.neuralAnalysis["configuration_changed"].toBool(), "analysis metadata change history");
    TimestampContinuityAnalyzer continuity(1); continuity.process(capture.bytes);
    require(continuity.stats().discontinuities == 0 && continuity.stats().intraFrameMismatchFrames == 0,
            "recorded timestamp continuity across all frames");
    const auto before = hub.statistics().distributedFrames;
    require(waitFor([&] { drain(queue, &received); return hub.statistics().distributedFrames > before; }), "record stop preserves live acquisition");
    require(hub.startRecording(root, name + "_restart", 0), "restart recording");
    require(waitFor([&] { return hub.recordedBytes() >= 320LL * kFrameBytes; }), "second recording receives");
    const QString restartPath = hub.recordingPath(); hub.stop();
    require(hub.state() == SessionHub::State::Idle && !hub.isConnected() && !hub.isRecording(), "session stop drains source/recorder");
    require(readCapture(restartPath).manifest.complete, "session-stop recording finalized");
    const quint64 epoch = hub.timelineEpoch();
    require(hub.start("127.0.0.1", control, data), "same endpoint reconnect");
    require(waitFor([&] { return hub.state() == SessionHub::State::Live && hub.statistics().distributedFrames >= 160; }), "reconnect receives");
    require(hub.timelineEpoch() > epoch, "reconnect invalidates prior timeline"); hub.stop();
    pass(dualPort ? "A02" : "A01", QString("%1 frames; %2 split parts; byte-exact raw; metadata/history; stop/restart/reconnect")
         .arg(capture.manifest.totalFrames).arg(capture.manifest.parts.size()));
    return capture;
}

void replayCases(const Capture &capture, const QString &root) {
    SessionHub hub; auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(512); hub.addSubscriber(queue);
    bool finished = false; qint64 position = -1, total = -1; QJsonObject restored;
    QObject::connect(&hub, &SessionHub::replayFinished, &hub, [&] { finished = true; });
    QObject::connect(&hub, &SessionHub::replayPosition, &hub, [&](qint64 p, qint64 t) { position = p; total = t; });
    QObject::connect(&hub, &SessionHub::replayAnalysisMetadataAvailable, &hub, [&](const QJsonObject &v) { restored = v; });
    const QString first = QDir(capture.path).filePath(capture.manifest.parts.first().fileName);
    hub.tdmContext()->setState(false, true);
    require(hub.startReplay(first, 1234.0), "load split-session first part");
    spin(30);
    require(hub.state() == SessionHub::State::ReplayReady && queue->size() == 0 && hub.sampleRate() == 20000.0,
            "load remains paused and manifest overrides fallback rate");
    require(hub.tdmContext()->enabled() && !hub.tdmContext()->pair02() && restored["configuration_changed"].toBool(),
            "replay restores TDM and analysis metadata");
    require(total == capture.manifest.totalFrames && position == 0, "replay full multipart frame count");
    require(hub.startRecording(root, "rerecorded_replay", 128LL * kFrameBytes), "record replay source");
    const QString rerecordedPath = hub.recordingPath();
    QByteArray replayed; hub.replayTogglePlay();
    require(waitFor([&] { drain(queue, &replayed); return finished; }), "replay finishes");
    require(waitFor([&] { drain(queue, &replayed); return replayed.size() >= capture.bytes.size(); }), "replay distributor drains");
    require(replayed == capture.bytes, "replayed all parts byte-for-byte");
    hub.stopRecording();
    const Capture rerecorded = readCapture(rerecordedPath);
    require(rerecorded.bytes == capture.bytes && rerecorded.manifest.complete &&
            rerecorded.manifest.metadata.source == "replay", "replay rerecording is byte-exact with replay provenance");
    pass("R01", QString("%1 raw bytes replayed identically; paused load; metadata restored").arg(replayed.size()));
    quint64 epoch = hub.timelineEpoch(); hub.replaySeekFraction(0.5);
    const qint64 mid = capture.manifest.totalFrames / 2;
    require(waitFor([&] { return position == mid; }) && queue->size() == 0 && hub.timelineEpoch() > epoch, "seek sets midpoint and clears queue epoch");
    require(hub.state() == SessionHub::State::ReplayPaused, "seek while paused stays paused");
    QByteArray afterSeek; hub.replayTogglePlay();
    require(waitFor([&] { drain(queue, &afterSeek); return !afterSeek.isEmpty(); }), "post-seek data");
    hub.replayPause();
    require(afterSeek == capture.bytes.mid(mid * kFrameBytes, afterSeek.size()), "no stale pre-seek bytes");
    spin(25); QByteArray pending; drain(queue, &pending); const auto stable = hub.statistics().distributedFrames;
    spin(35); require(hub.statistics().distributedFrames == stable, "pause stops new frames");
    hub.replayJumpStart(); require(waitFor([&] { return position == 0; }), "jump start");
    hub.replaySkip(0.01); require(waitFor([&] { return position == 200; }), "skip forward exact source-rate frames");
    hub.replaySkip(-100.0); require(waitFor([&] { return position == 0; }), "skip clamps at start");
    hub.replayJumpEnd(); require(waitFor([&] { return position == total; }), "jump end");
    hub.replaySeekFraction(-1.0); require(waitFor([&] { return position == 0; }), "seek lower bound");
    hub.replaySeekFraction(2.0); require(waitFor([&] { return position == total; }), "seek upper bound");
    hub.stop(); require(!hub.isReplaying() && hub.state() == SessionHub::State::Idle, "unload replay");
    pass("R02", "play/pause/seek/jump/skip/clamping and epoch barrier verified against recorded bytes");
    require(!hub.startReplay(QDir(root).filePath("missing.bin"), 20000.0) && hub.state() == SessionHub::State::Error,
            "missing replay errors");
    QString badDir = QDir(root).filePath("malformed"); QDir().mkpath(badDir);
    writeFile(QDir(badDir).filePath("ADC_DATA.bin"), capture.bytes);
    writeFile(QDir(badDir).filePath("session.json"), "{broken");
    require(!hub.startReplay(QDir(badDir).filePath("ADC_DATA.bin"), 20000.0), "malformed manifest cannot silently fallback");
    require(hub.startReplay(first, 20000.0), "valid load recovers from replay error"); hub.stop();
    pass("R03", "missing file and malformed manifest fail; subsequent valid replay recovers");
    auto blockedQueue = std::make_shared<ThreadSafeQueue<QByteArray>>(1);
    require(blockedQueue->push(QByteArray(kFrameBytes, '\0')), "backpressure filler");
    ReplayController blocked(blockedQueue); bool backpressure = false;
    QObject::connect(&blocked, &ReplayController::backpressureChanged, &blocked, [&](bool value) { backpressure |= value; });
    require(blocked.open(first, 20000.0), "backpressure opens captured Dummy recording");
    blocked.play();
    require(waitFor([&] { return backpressure; }), "backpressure visible");
    require(blocked.currentFrame() == 0, "full queue does not consume source frames");
    QByteArray filler; blockedQueue->pop(filler, 0);
    require(waitFor([&] { return blocked.currentFrame() > 0; }), "backpressure resumes after drain");
    QByteArray resumed; require(blockedQueue->pop(resumed, 0), "resumed source bytes");
    require(resumed == capture.bytes.left(resumed.size()), "backpressure did not skip any source bytes");
    blocked.close();
    pass("R04", "full subscriber queue pauses replay without advancing; drain resumes byte-exact source prefix");
}

void exportCases(const Capture &capture, const QString &root) {
    const QString first = QDir(capture.path).filePath(capture.manifest.parts.first().fileName);
    const qint64 frames = capture.manifest.totalFrames;
    for (auto mode : {MatlabExportMode::RawAdc, MatlabExportMode::RawWords, MatlabExportMode::TdmDemux}) {
        MatlabChannelExportOptions options; options.mode = mode; options.evenSampleIsFirstLocalEle = false;
        const QString folder = QDir(root).filePath(QString("export_%1").arg(int(mode)));
        auto result = MatlabChannelExporter::exportAdcBin(first, folder, capture.manifest.metadata.sampleRate, options);
        require(result.ok && result.frameCount == frames && result.leftoverBytes == 0 &&
                result.outputCount == (mode == MatlabExportMode::TdmDemux ? 1024 : 256), "export output/frame count: " + result.error);
        qint64 checked = 0;
        for (int ch = 0; ch < 256; ++ch) {
            const int phases = mode == MatlabExportMode::TdmDemux ? 4 : 1;
            for (int phase = 0; phase < phases; ++phase) {
                QString filename;
                if (mode == MatlabExportMode::TdmDemux)
                    filename = QString("ele%1_i32le.bin").arg(ch * 4 + phase, 4, 10, QLatin1Char('0'));
                else filename = QString(mode == MatlabExportMode::RawWords ? "ch%1_raw32le.bin" : "ch%1_i32le.bin").arg(ch, 3, 10, QLatin1Char('0'));
                QByteArray expected;
                for (qint64 f = 0; f < frames; ++f) {
                    if (mode == MatlabExportMode::TdmDemux && ((capture.manifest.metadata.frameOrigin + f) % 4 != phase)) continue;
                    quint32 raw = wordAt(capture.bytes, f, ch);
                    quint32 value = mode == MatlabExportMode::RawWords ? raw : raw & kAdcSampleMask;
                    char le[4]; qToLittleEndian<quint32>(value, le); expected.append(le, 4); ++checked;
                }
                require(readFile(QDir(folder).filePath(filename)) == expected, "all exported samples match source: " + filename);
            }
        }
        if (mode == MatlabExportMode::TdmDemux) {
            for (int phase = 0; phase < 4; ++phase) {
                QByteArray expected;
                for (qint64 f = 0; f < frames; ++f) if ((capture.manifest.metadata.frameOrigin + f) % 4 == phase) {
                    char le[8]; qToLittleEndian<qint64>(capture.manifest.metadata.frameOrigin + f, le); expected.append(le, 8);
                }
                require(readFile(QDir(folder).filePath(QString("tdm_phase%1_frames_i64le.bin").arg(phase))) == expected,
                        "exact TDM source timeline");
            }
            require(readFile(QDir(folder).filePath("read_tdm_timeline.m")).contains("function"), "TDM MATLAB helper created");
        } else require(readFile(QDir(folder).filePath("load_adc_channels.m")).contains("20000"), "MATLAB loader uses explicit GUI-prefilled sample rate");
        pass(QString("E0%1").arg(int(mode) + 1), QString("%1 output files; %2 samples compared exactly%3")
             .arg(result.outputCount).arg(checked).arg(mode == MatlabExportMode::TdmDemux ? "; all four TDM timeline files exact" : ""));
    }
    std::atomic_bool cancelled{true}; MatlabChannelExportOptions options; options.cancelFlag = &cancelled;
    auto cancel = MatlabChannelExporter::exportAdcBin(first, QDir(root).filePath("cancelled_export"), 20000.0, options);
    require(!cancel.ok && !cancel.error.isEmpty(), "cancelled export reported failure");
    auto missing = MatlabChannelExporter::exportAdcBin(QDir(root).filePath("absent.bin"), QDir(root).filePath("invalid_export"), 20000.0);
    require(!missing.ok && !missing.error.isEmpty(), "missing export input error");
    pass("E04", "cancelled and missing-input exports return explicit errors");
}

void triggerCases(const QString &root) {
    int endpoint = port(); DummyStreamServer dummy; QString error;
    require(dummy.start(endpoint, endpoint, &error), "trigger Dummy start");
    SessionHub hub;
    require(hub.start("127.0.0.1", endpoint, endpoint), "trigger live start");
    require(waitFor([&] { return hub.state() == SessionHub::State::Live && hub.statistics().distributedFrames > 160; }), "trigger live ready");
    int starts = 0; int stale = 0; QString captureName;
    QObject::connect(&hub, &SessionHub::triggerFired, &hub, [&](quint64 epoch) {
        if (epoch != hub.timelineEpoch()) { ++stale; return; }
        if (!hub.isRecording() && hub.state() == SessionHub::State::Live) {
            if (hub.startRecording(root, captureName)) ++starts;
        }
    });
    for (int scenario = 0; scenario < 6; ++scenario) {
        bool tdm = scenario >= 2; bool pair02 = scenario < 4; int slot = tdm ? scenario % 2 : -1;
        hub.tdmContext()->setState(tdm, pair02);
        SessionHub::TriggerConfig trigger; trigger.enabled = true; trigger.channel = 0;
        trigger.thresholdV = 0.9; trigger.risingAbove = scenario != 1; trigger.tdmSlot = slot;
        captureName = QString("trigger_%1").arg(scenario); const int before = starts;
        hub.setTriggerConfig(trigger);
        require(waitFor([&] { return starts == before + 1 && hub.recordedBytes() >= 320LL * kFrameBytes; }), "threshold trigger recording started");
        spin(35); require(starts == before + 1, "no duplicate trigger record while recording");
        trigger.enabled = false; hub.setTriggerConfig(trigger); hub.stopRecording();
        const auto recorded = readCapture(hub.recordingPath());
        require(recorded.manifest.complete && recorded.manifest.totalFrames > 0, "trigger raw finalized");
        require(recorded.manifest.metadata.tdmEnabled == tdm && recorded.manifest.metadata.tdmEvenFirst == pair02,
                "trigger TDM metadata");
    }
    hub.stop();
    require(stale == 0, "all trigger callbacks match active epoch");
    pass("T01", "six live sine scenarios: rising/falling plus TDM phases 0/2/1/3; exactly one active recording per trigger");
}

void errorCases(const QString &root) {
    SessionHub hub;
    QObject::connect(&hub, &SessionHub::recordingError, &hub, [&](const QString &message) {
        std::cerr << "DIAGNOSTIC recordingError active=" << hub.isRecording()
                  << " bytes=" << hub.recordedBytes() << " path=" << hub.recordingPath().toStdString()
                  << " message=" << message.toStdString() << std::endl;
    });
    QObject::connect(&hub, &SessionHub::recordingStateChanged, &hub, [&](bool active, const QString &path) {
        std::cerr << "DIAGNOSTIC recordingState=" << active << " path=" << path.toStdString() << std::endl;
    }); const int dead = port();
    require(hub.start("127.0.0.1", dead, dead), "unreachable attempt admitted");
    require(waitFor([&] { return hub.state() == SessionHub::State::Error; }, 7000), "connection refusal -> Error");
    const int endpoint = port(); auto dummy = std::make_unique<DummyStreamServer>(); QString error;
    require(dummy->start(endpoint, endpoint, &error), "recovery Dummy listen");
    require(hub.start("127.0.0.1", endpoint, endpoint), "recover start from Error");
    require(waitFor([&] { return hub.state() == SessionHub::State::Live && hub.statistics().distributedFrames > 160; }), "recovery live data");
    const QString fileInsteadOfDirectory = QDir(root).filePath("not_a_directory"); writeFile(fileInsteadOfDirectory, "x");
    require(!hub.startRecording(fileInsteadOfDirectory, "invalid") && !hub.isRecording(), "unwritable parent returns false/no recording");
    require(hub.startRecording(root, "interrupted"), "valid recording after failed directory");
    require(waitFor([&] { return hub.recordedBytes() >= 320LL * kFrameBytes; }), "interruption fixture recorded");
    const QString path = hub.recordingPath(); dummy.reset();
    require(waitFor([&] { return hub.state() == SessionHub::State::Error; }, 7000), "source disconnect becomes Error");
    require(!hub.isRecording() && !hub.isConnected(), "disconnect stops recorder and data connection");
    const Capture interrupted = readCapture(path);
    require(!interrupted.manifest.active && !interrupted.manifest.complete && !interrupted.manifest.stopReason.isEmpty(),
            "unexpected disconnect finalized as incomplete");
    hub.stop();
    pass("A03", "connection refusal/recovery; invalid recording directory; mid-record source disconnect -> incomplete manifest");
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    QTemporaryDir temporary;
    try {
        require(temporary.isValid(), "temporary directory");
        if (app.arguments().contains("--errors-only")) {
            errorCases(temporary.path());
            return 0;
        }
        controlCases(temporary.path());
        simulatedControlState(temporary.path());
        Capture single = liveCapture(temporary.path(), false);
        liveCapture(temporary.path(), true);
        replayCases(single, temporary.path());
        exportCases(single, temporary.path());
        triggerCases(temporary.path());
        errorCases(temporary.path());
        std::cout << "PASS dummy_acquisition_e2e: software/protocol simulation only; no physical hardware claim" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL dummy_acquisition_e2e: " << error.what() << std::endl;
        return 1;
    }
}
