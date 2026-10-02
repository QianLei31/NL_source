#include "service/dummy_stream_server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <iostream>

namespace {
constexpr int kFrameBytes = 256 * 4;

bool check(bool result, const char *what)
{
    if (!result) std::cerr << "FAIL: " << what << std::endl;
    return result;
}

bool waitFor(const std::function<bool()> &predicate, int ms = 2000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (predicate()) return true;
        QThread::msleep(1);
    } while (timer.elapsed() < ms);
    return predicate();
}

void settle(int ms = 20)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

int availablePort()
{
    QTcpServer socket;
    return socket.listen(QHostAddress::LocalHost, 0) ? socket.serverPort() : 0;
}

bool connect(QTcpSocket &socket, int port)
{
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    return waitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; });
}

QByteArray spi(unsigned opcode, unsigned channel = 0, quint16 value = 0)
{
    const quint32 word = (opcode << 26) | (channel << 16) | value;
    return "spi" + QByteArray::number(word, 16).rightJustified(8, '0');
}

bool exchange(QTcpSocket &socket, const QByteArray &commands, int count = 1)
{
    socket.write(commands);
    if (!waitFor([&] { return socket.bytesAvailable() >= count * 12; })) return false;
    return socket.read(count * 12) == QByteArray(count * 12, '\0');
}

QByteArray capture(int port, int frames = 320, bool fragmentedStart = false)
{
    QTcpSocket socket;
    if (!connect(socket, port)) return {};
    socket.write(fragmentedStart ? "ct" : "ctre");
    if (fragmentedStart) {
        settle();
        if (socket.bytesAvailable() != 0) return {};
        socket.write("re");
    }
    if (!waitFor([&] { return socket.bytesAvailable() >= frames * kFrameBytes; })) return {};
    const QByteArray result = socket.read(frames * kFrameBytes);
    socket.write("stop");
    waitFor([&] { return socket.state() == QAbstractSocket::UnconnectedState; });
    settle();
    return result;
}

int adc(const QByteArray &bytes, int frame, int channel)
{
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData()) +
                    frame * kFrameBytes + channel * 4;
    return static_cast<int>(qFromLittleEndian<quint32>(p) & 0xfff);
}

bool checkStateAndFraming(ccv2::DummyStreamServer &server, QTcpSocket &control, int port)
{
    auto initial = server.protocolState();
    for (int ch = 0; ch < 256; ++ch) {
        if (!check(initial.rec[ch] == 0x060 && initial.dac[ch] == 0x8000 &&
                   initial.dacCt[ch] == 0 && server.channelGain(ch) == 60.0,
                   "documented simulator defaults")) return false;
    }
    if (!check(!initial.analogResetAsserted.has_value() && !initial.globalDacEnabled.has_value() &&
               !initial.cbokLow.has_value(), "global defaults explicitly unknown")) return false;
    if (!check(server.channelGain(-1) == 0 && !server.channelSignalEnabled(256),
               "invalid channel guarded")) return false;

    // Exact GUI wire layout: six opcode, eight block, two local channel bits.
    // Test every possible TCP split of the eleven-byte request.
    for (int split = 1; split < 11; ++split) {
        const QByteArray command = spi(0x04, 173, static_cast<quint16>(0x840 + split));
        const auto before = server.protocolState().spiCommands;
        control.write(command.left(split));
        settle(3);
        if (!check(server.protocolState().spiCommands == before && control.bytesAvailable() == 0,
                   "partial SPI must not apply or acknowledge")) return false;
        control.write(command.mid(split));
        if (!waitFor([&] { return control.bytesAvailable() >= 12; }) ||
            !check(control.read(12) == QByteArray(12, '\0'), "legacy placeholder reply")) return false;
        if (!check(server.protocolState().rec[173] == 0x840 + split,
                   "fragmented REC write resolves correct address")) return false;
    }
    const auto before = server.protocolState().spiCommands;
    if (!exchange(control, spi(0x04, 173, 0xff40) + "\r\n" +
                           spi(0x06, 255, 0x6dff) + spi(0x1e, 255, 0xffab) + spi(0), 4)) return false;
    auto state = server.protocolState();
    if (!check(state.spiCommands == before + 4 && state.rec[173] == 0x0f40 &&
               state.dac[255] == 0x6dff && state.dacCt[255] == 0xffab &&
               state.rec[172] == 0x060 && state.dac[254] == 0x8000,
               "coalesced writes mask REC, preserve DAC fields, isolate channels")) return false;

    for (unsigned opcode : {0x07u, 0x08u, 0x09u, 0x0du, 0x12u}) {
        if (!exchange(control, spi(opcode))) return false;
        state = server.protocolState();
        if (!check(state.lastCommandSupported, "documented exact global accepted")) return false;
        if (opcode == 0x07 && !check(state.analogResetAsserted == true, "reset asserted")) return false;
        if (opcode == 0x08 && !check(state.analogResetAsserted == false, "reset released")) return false;
        if (opcode == 0x09 && !check(state.globalDacEnabled == true, "global DAC enabled")) return false;
        if (opcode == 0x0d && !check(state.globalDacEnabled == false, "global DAC disabled")) return false;
        if (opcode == 0x12 && !check(state.cbokLow == true, "CBOK low recorded")) return false;
    }
    const auto preserved = server.protocolState();
    if (!exchange(control, spi(0x04, 256, 0) + spi(0x3f) + spi(0x09, 0, 1), 3)) return false;
    state = server.protocolState();
    if (!check(state.unsupportedCommands == preserved.unsupportedCommands + 3 &&
               !state.lastCommandSupported && state.rec == preserved.rec &&
               state.dac == preserved.dac && state.dacCt == preserved.dacCt &&
               state.globalDacEnabled == preserved.globalDacEnabled,
               "unsupported address/opcode/global payload cannot mutate known state")) return false;

    // Global gain is the actual GUI sequence: 256 REC writes plus 256 no-ops.
    for (quint16 reg : {quint16(0x005e), quint16(0x007e)}) {
        QByteArray commands;
        for (int ch = 0; ch < 256; ++ch) commands += spi(0x04, ch, reg) + spi(0);
        if (!exchange(control, commands, 512)) return false;
        state = server.protocolState();
        for (int ch = 0; ch < 256; ++ch) {
            if (!check(state.rec[ch] == reg && server.channelGain(ch) == ((reg & 0x20) ? 60 : 180),
                       "all 256 gain writes applied through real TCP")) return false;
        }
    }
    for (const QByteArray bad : {QByteArray("spizz000000"), QByteArray("garbage"), QByteArray("spi12")}) {
        const auto count = server.protocolState().malformedCommands;
        QTcpSocket broken;
        if (!connect(broken, port)) return false;
        broken.write(bad);
        settle();
        broken.disconnectFromHost();
        if (!waitFor([&] { return server.protocolState().malformedCommands > count; })) return false;
        if (!check(server.protocolState().malformedCommands == count + 1,
                   "malformed/incomplete request counted exactly once")) return false;
    }
    return check(exchange(control, spi(0x04, 0, 0x060)), "valid client survives malformed peer");
}

bool checkSignalEffects(ccv2::DummyStreamServer &server, QTcpSocket &control, int port)
{
    QByteArray resets;
    for (int ch = 0; ch < 256; ++ch) resets += spi(0x04, ch, 0x060);
    if (!exchange(control, resets, 256)) return false;
    const QByteArray low = capture(port, 320, true);
    if (!check(low.size() == 320 * kFrameBytes, "fragmented ctre produces complete frames")) return false;
    if (!exchange(control, spi(0x04, 0, 0x040) + spi(0x04, 7, 0x040), 2)) return false;
    const QByteArray high = capture(port);
    if (!check(high.size() == low.size(), "gain-modified stream received")) return false;
    int clipped = 0;
    for (int frame = 0; frame < 320; ++frame) {
        if (!check(std::abs((adc(high, frame, 0) - 2048) - 3 * (adc(low, frame, 0) - 2048)) <= 2,
                   "60x to 180x AC gain with one-count quantization")) return false;
        if (!check(adc(high, frame, 1) == adc(low, frame, 1), "gain isolated to addressed channel")) return false;
        if (adc(high, frame, 7) == 4095) ++clipped;
        const auto *p = reinterpret_cast<const uchar *>(high.constData()) + frame * kFrameBytes;
        if (!check((qFromLittleEndian<quint32>(p) >> 12) == static_cast<quint32>(frame),
                   "gain does not corrupt timestamps")) return false;
    }
    if (!check(clipped > 0, "high gain clips to 12-bit ADC range")) return false;
    if (!exchange(control, spi(0x04, 0, 0x440))) return false;
    const QByteArray off = capture(port);
    if (off.size() != low.size()) return false;
    for (int frame = 0; frame < 320; ++frame) {
        if (!check(adc(off, frame, 0) == 2048 && adc(off, frame, 1) == adc(low, frame, 1),
                   "REC OFF suppresses synthetic AC only on target")) return false;
    }
    if (!check(server.protocolState().rec[0] == 0x440,
               "ctre restarts waveform but preserves register state")) return false;
    if (!exchange(control, spi(0x04, 0, 0x060))) return false;
    server.setTdmSimulation(true);
    const QByteArray tdm = capture(port);
    if (tdm.size() != low.size()) return false;
    for (int frame = 0; frame < 320; ++frame) {
        const double scale = 1.0 + 0.25 * (frame % 4);
        if (!check(std::abs((adc(tdm, frame, 0) - 2048) - scale * (adc(low, frame, 0) - 2048)) <= 2,
                   "opt-in four-phase fixture has explicit deterministic amplitude")) return false;
        if (!check(server.simulatedElectrode(255, frame) == 1020 + frame % 4,
                   "TDM source-frame route covers phases zero through three")) return false;
    }
    if (!check(server.simulatedElectrode(256, 0) == -1 &&
               server.simulatedElectrode(1, (1u << 20)) == 4,
               "TDM route bounded and phase survives timestamp wrap")) return false;
    server.setTdmSimulation(false);
    server.setWaveform(ccv2::DummyWaveform::Spike);
    const QByteArray spikeLow = capture(port);
    if (!exchange(control, spi(0x04, 0, 0x040))) return false;
    const QByteArray spikeHigh = capture(port);
    if (spikeLow.size() != low.size() || spikeHigh.size() != low.size()) return false;
    bool nonzero = false;
    for (int frame = 0; frame < 320; ++frame) {
        nonzero |= adc(spikeHigh, frame, 0) != 2048;
        if (!check(std::abs((adc(spikeHigh, frame, 0) - 2048) -
                           3 * (adc(spikeLow, frame, 0) - 2048)) <= 2,
                   "input-referred spike/noise generator obeys REC gain")) return false;
        if (!check(adc(spikeHigh, frame, 1) == adc(spikeLow, frame, 1),
                   "spike gain does not change other channel RNG sequence")) return false;
    }
    return check(nonzero, "spike gain check has nonzero synthetic samples");
}

bool checkDualPort()
{
    const int controlPort = availablePort();
    const int dataPort = availablePort();
    if (!controlPort || !dataPort || controlPort == dataPort) return false;
    ccv2::DummyStreamServer server;
    QString error;
    if (!server.start(controlPort, dataPort, &error)) return false;
    QTcpSocket control, data;
    if (!connect(control, controlPort) || !connect(data, dataPort)) return false;
    settle();
    if (!check(data.bytesAvailable() == 0, "dual data waits for ctre")) return false;
    if (!exchange(control, spi(0x04, 19, 0x040))) return false;
    control.write("ctre");
    if (!waitFor([&] { return data.bytesAvailable() >= kFrameBytes; })) return false;
    if (!check(server.channelGain(19) == 180, "dual stream start preserves programmed gain")) return false;
    control.write("stop");
    // A control write and already queued stream payload can take different
    // TCP paths. Observe sustained quiescence after draining in-flight data,
    // rather than assuming the command reached the peer in a fixed 20 ms.
    QElapsedTimer quiet;
    quiet.start();
    if (!check(waitFor([&] {
            if (!data.readAll().isEmpty()) quiet.restart();
            return quiet.elapsed() >= 100;
        }), "dual stop disarms generation")) return false;
    control.write("ctre");
    if (!waitFor([&] { return data.bytesAvailable() >= kFrameBytes; })) return false;
    const QByteArray first = data.read(kFrameBytes);
    return check((qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(first.constData())) >> 12) == 0,
                 "dual restart resets timestamp");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int port = availablePort();
    ccv2::DummyStreamServer server;
    QString error;
    if (!port || !server.start(port, port, &error)) return 1;
    QTcpSocket control;
    if (!connect(control, port)) return 2;
    if (!checkStateAndFraming(server, control, port)) return 3;
    if (!checkSignalEffects(server, control, port)) return 4;
    if (!checkDualPort()) return 5;
    std::cout << "dummy_protocol_model_smoke PASS: SIMULATED state, TCP framing, gain, OFF, clipping, TDM routing, dual lifecycle; no hardware readback claim" << std::endl;
    return 0;
}
