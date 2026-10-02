#include "service/dummy_stream_server.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QtEndian>

#include <cmath>

#include "core/constants.h"

namespace ccv2 {

namespace {

constexpr int kBatchFrames = 160;
constexpr int kBatchIntervalMs = 8;
constexpr qint64 kMaxPendingBytes = 4 * 1024 * 1024;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr quint32 kTimestampMask = (1u << 20) - 1u;
constexpr double kAdcFullScaleV = 1.8;
constexpr double kAdcLsbV = kAdcFullScaleV / 4096.0;
// Match the hardware's default REC gain (bit 5 = 1). The Spike page can use
// 180x when the hardware is configured for its high-gain mode.
constexpr double kDummyFrontEndGain = 60.0;
constexpr double kSpikeNoiseUv = 12.0;
constexpr double kSpikeMinUv = 80.0;
constexpr double kSpikeMaxUv = 420.0;
constexpr double kSpikeMinRateHz = 2.0;
constexpr double kSpikeMaxRateHz = 45.0;

double inputUvToAdcCounts(double inputUv)
{
    return inputUv * 1e-6 * kDummyFrontEndGain / kAdcLsbV;
}

bool isActiveSpikeChannel(int channel)
{
    // A deterministic, spatially scattered 55% population (140/256 channels).
    return ((channel * 73 + 19) % 100) < 55;
}

double channelUnitValue(int channel, int multiplier, int offset)
{
    return static_cast<double>((channel * multiplier + offset) % 1000) / 999.0;
}

int argumentPort(const QStringList &arguments, const QString &name, int fallback)
{
    const QString prefix = name + QLatin1Char('=');
    for (const QString &argument : arguments) {
        if (!argument.startsWith(prefix)) {
            continue;
        }
        bool ok = false;
        const int value = argument.mid(prefix.size()).toInt(&ok);
        if (ok && value > 0 && value <= 65535) {
            return value;
        }
    }
    return fallback;
}

}  // namespace

DummyStreamServer::DummyStreamServer(QObject *parent)
    : QObject(parent),
      m_controlServer(new QTcpServer(this)),
      m_dataServer(new QTcpServer(this)),
      m_streamTimer(new QTimer(this))
{
    m_streamTimer->setTimerType(Qt::PreciseTimer);
    m_streamTimer->setInterval(kBatchIntervalMs);
    connect(m_streamTimer, &QTimer::timeout, this, &DummyStreamServer::sendBatch);
    m_protocol.rec.fill(0x0060);
    m_protocol.dac.fill(0x8000);
    m_protocol.dacCt.fill(0x0000);
    resetPhases();
}

DummyStreamServer::~DummyStreamServer()
{
    m_streamTimer->stop();
    const auto streamClients = m_streamClients.values();
    const auto controlClients = m_controlClients.values();
    for (QTcpSocket *socket : streamClients) {
        disconnect(socket, nullptr, nullptr, nullptr);
        socket->abort();
    }
    for (QTcpSocket *socket : controlClients) {
        disconnect(socket, nullptr, nullptr, nullptr);
        socket->abort();
    }
    m_streamClients.clear();
    m_controlClients.clear();
    m_controlServer->close();
    m_dataServer->close();
}

bool DummyStreamServer::start(int controlPort, int dataPort, QString *error)
{
    m_singlePort = controlPort == dataPort;
    m_dualPortArmed = m_singlePort;

    if (!m_controlServer->listen(QHostAddress::LocalHost,
                                 static_cast<quint16>(controlPort))) {
        const QString message = QStringLiteral("Control port %1 unavailable: %2")
                                    .arg(controlPort)
                                    .arg(m_controlServer->errorString());
        if (error) {
            *error = message;
        }
        emit serverError(message);
        return false;
    }

    if (!m_singlePort &&
        !m_dataServer->listen(QHostAddress::LocalHost,
                              static_cast<quint16>(dataPort))) {
        const QString message = QStringLiteral("Data port %1 unavailable: %2")
                                    .arg(dataPort)
                                    .arg(m_dataServer->errorString());
        m_controlServer->close();
        if (error) {
            *error = message;
        }
        emit serverError(message);
        return false;
    }

    if (m_singlePort) {
        connect(m_controlServer, &QTcpServer::newConnection,
                this, &DummyStreamServer::acceptSinglePortClients);
    } else {
        connect(m_controlServer, &QTcpServer::newConnection,
                this, &DummyStreamServer::acceptControlClients);
        connect(m_dataServer, &QTcpServer::newConnection,
                this, &DummyStreamServer::acceptDataClients);
    }

    m_streamTimer->start();
    return true;
}

void DummyStreamServer::acceptSinglePortClients()
{
    acceptControlClients();
}

void DummyStreamServer::acceptControlClients()
{
    while (m_controlServer->hasPendingConnections()) {
        QTcpSocket *socket = m_controlServer->nextPendingConnection();
        socket->setReadBufferSize(4096);
        m_controlClients.insert(socket);
        m_commandBuffers.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket]() { readControlCommands(socket); });
        connect(socket, &QTcpSocket::disconnected, socket,
                [this, socket]() { removeClient(socket); });
    }
}

void DummyStreamServer::rejectMalformed(QTcpSocket *socket, const QString &reason)
{
    ++m_protocol.malformedCommands;
    m_protocol.lastCommandSupported = false;
    m_protocol.lastMessage = reason;
    m_commandBuffers.remove(socket);
    // No invented success/error wire frame for malformed input.
    socket->disconnectFromHost();
}

void DummyStreamServer::readControlCommands(QTcpSocket *socket)
{
    QByteArray &buffer = m_commandBuffers[socket];
    buffer.append(socket->readAll());
    // At most one partial 11-byte command can survive each read; Qt caps
    // each socket read at 4096 bytes. No unbounded partial-input accumulation.
    if (buffer.size() > 4106) {
        rejectMalformed(socket, QStringLiteral("SIMULATED: command buffer limit"));
        return;
    }
    while (!buffer.isEmpty()) {
        while (!buffer.isEmpty() && (buffer[0] == ' ' || buffer[0] == '\t' ||
               buffer[0] == '\r' || buffer[0] == '\n')) buffer.remove(0, 1);
        if (buffer.isEmpty()) return;
        const QByteArray prefix = buffer.left(4).toLower();
        if (prefix.startsWith("spi")) {
            if (buffer.size() < 11) return;
            const QByteArray hex = buffer.mid(3, 8);
            for (const char c : hex) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) {
                    rejectMalformed(socket, QStringLiteral("SIMULATED: SPI requires eight hexadecimal digits"));
                    return;
                }
            }
            if (m_singlePort && m_streamClients.contains(socket)) {
                rejectMalformed(socket, QStringLiteral("SIMULATED: use a separate SPI control connection"));
                return;
            }
            const quint32 word = hex.toUInt(nullptr, 16);
            buffer.remove(0, 11);
            applySpiWord(word);
            // Preserve the old Dummy's 12 zero bytes for GUI transport
            // compatibility. This is NOT a register echo or board response.
            socket->write(QByteArray(12, '\0'));
        } else if (prefix == "ctre") {
            buffer.remove(0, 4);
            if (m_singlePort) {
                const bool firstStream = m_streamClients.isEmpty();
                m_controlClients.remove(socket);
                m_streamClients.insert(socket);
                if (firstStream) resetPhases();
            } else {
                m_dualPortArmed = true;
                resetPhases();
            }
        } else if (prefix == "stop") {
            buffer.remove(0, 4);
            if (m_singlePort) {
                socket->disconnectFromHost();
                return;
            }
            m_dualPortArmed = false;
        } else if (QByteArray("spi").startsWith(prefix) ||
                   QByteArray("ctre").startsWith(prefix) ||
                   QByteArray("stop").startsWith(prefix)) {
            return; // TCP fragment, not a command boundary.
        } else {
            rejectMalformed(socket, QStringLiteral("SIMULATED: unknown transport command"));
            return;
        }
    }
}

void DummyStreamServer::applySpiWord(quint32 word)
{
    ++m_protocol.spiCommands;
    m_protocol.lastSpiWord = word;
    const unsigned opcode = word >> 26;
    const unsigned block = (word >> 18) & 0xff;
    const unsigned channel = (word >> 16) & 3;
    const quint16 data = static_cast<quint16>(word & 0xffff);
    bool supported = false;
    if ((opcode == 0x04 || opcode == 0x06 || opcode == 0x1e) && block < 64) {
        const unsigned index = block * 4 + channel;
        if (opcode == 0x04) m_protocol.rec[index] = data & 0x0fff;
        if (opcode == 0x06) m_protocol.dac[index] = data;
        if (opcode == 0x1e) m_protocol.dacCt[index] = data;
        supported = true;
    } else if ((word & 0x03ffffffu) == 0) {
        // Only the exact global words shipped in the actual GUI are known.
        switch (opcode) {
        case 0x00: supported = true; break;
        case 0x07: m_protocol.analogResetAsserted = true; supported = true; break;
        case 0x08: m_protocol.analogResetAsserted = false; supported = true; break;
        case 0x09: m_protocol.globalDacEnabled = true; supported = true; break;
        case 0x0d: m_protocol.globalDacEnabled = false; supported = true; break;
        case 0x12: m_protocol.cbokLow = true; supported = true; break;
        default: break;
        }
    }
    m_protocol.lastCommandSupported = supported;
    if (supported) ++m_protocol.supportedCommands;
    else ++m_protocol.unsupportedCommands;
    m_protocol.lastMessage = supported
        ? QStringLiteral("SIMULATED: documented command stored; zero reply is compatibility-only")
        : QStringLiteral("SIMULATED: unsupported opcode/address/payload; state unchanged");
    emit protocolCommandProcessed(word, supported);
}

double DummyStreamServer::channelGain(int channel) const
{
    if (channel < 0 || channel >= kChannels) return 0.0;
    return (m_protocol.rec[channel] & (1u << 5)) ? 60.0 : 180.0;
}

bool DummyStreamServer::channelSignalEnabled(int channel) const
{
    return channel >= 0 && channel < kChannels &&
           !(m_protocol.rec[channel] & (1u << 10));
}

int DummyStreamServer::simulatedElectrode(int adcChannel, quint32 sourceFrame) const
{
    if (adcChannel < 0 || adcChannel >= kChannels) return -1;
    return m_tdmSimulation ? 4 * adcChannel + static_cast<int>(sourceFrame & 3u)
                           : adcChannel;
}

double DummyStreamServer::signalScale(int channel, quint32 sourceFrame) const
{
    if (!channelSignalEnabled(channel)) return 0.0;
    const double phaseScale = m_tdmSimulation ? 1.0 + 0.25 * (sourceFrame & 3u) : 1.0;
    return channelGain(channel) / kDummyFrontEndGain * phaseScale;
}

void DummyStreamServer::acceptDataClients()
{
    while (m_dataServer->hasPendingConnections()) {
        QTcpSocket *socket = m_dataServer->nextPendingConnection();
        m_streamClients.insert(socket);
        connect(socket, &QTcpSocket::disconnected, socket,
                [this, socket]() { removeClient(socket); });
    }
}

void DummyStreamServer::removeClient(QTcpSocket *socket)
{
    if (!m_commandBuffers.value(socket).trimmed().isEmpty()) {
        ++m_protocol.malformedCommands;
        m_protocol.lastCommandSupported = false;
        m_protocol.lastMessage = QStringLiteral("SIMULATED: incomplete command at disconnect");
    }
    m_commandBuffers.remove(socket);
    m_controlClients.remove(socket);
    m_streamClients.remove(socket);
    socket->deleteLater();
}

void DummyStreamServer::resetPhases()
{
    m_sinState.resize(kChannels);
    m_cosState.resize(kChannels);
    m_sinStep.resize(kChannels);
    m_cosStep.resize(kChannels);
    m_spikeCountdown.fill(0, kChannels);
    m_spikeRateHz.fill(0.0, kChannels);
    m_spikePeakCounts.fill(0.0, kChannels);
    m_timestamp = 0;
    m_rng.seed(0xC0FFEEu);
    for (int channel = 0; channel < kChannels; ++channel) {
        const double frequency =
            kBaseFrequencyHz + static_cast<double>(channel % 16) * kFrequencyStepHz;
        const double step = kTwoPi * frequency / static_cast<double>(kSampleRate);
        m_sinState[channel] = 0.0;
        m_cosState[channel] = 1.0;
        m_sinStep[channel] = std::sin(step);
        m_cosStep[channel] = std::cos(step);

        if (isActiveSpikeChannel(channel)) {
            const double rateMix = channelUnitValue(channel, 29, 7);
            const double ampMix = channelUnitValue(channel, 61, 23);
            m_spikeRateHz[channel] =
                kSpikeMinRateHz + rateMix * (kSpikeMaxRateHz - kSpikeMinRateHz);
            const double inputAmplitudeUv =
                kSpikeMinUv + ampMix * (kSpikeMaxUv - kSpikeMinUv);
            m_spikePeakCounts[channel] = inputUvToAdcCounts(inputAmplitudeUv);
        }
    }
}

namespace {

// Biphasic extracellular action-potential shape over t in [0,1):
// sharp negative trough followed by a smaller positive rebound.
double spikeShape(double t)
{
    const double trough = std::exp(-((t - 0.22) * (t - 0.22)) / (2.0 * 0.06 * 0.06));
    const double rebound = std::exp(-((t - 0.52) * (t - 0.52)) / (2.0 * 0.12 * 0.12));
    return -trough + 0.35 * rebound;   // peak magnitude ~ -1.0
}

}  // namespace

void DummyStreamServer::sendBatch()
{
    if ((!m_singlePort && !m_dualPortArmed) || m_streamClients.isEmpty()) {
        return;
    }

    QByteArray payload(kBatchFrames * kFrameBytes, Qt::Uninitialized);
    char *output = payload.data();

    if (m_waveform == DummyWaveform::Spike) {
        // Neural-style data modeled after tools/dummy_spike_tcp_server.py:
        // a fixed active/silent population, per-channel rate and amplitude,
        // Gaussian input noise, and sparse biphasic extracellular spikes.
        // Microvolt values are passed through an explicit dummy front-end gain
        // before 12-bit quantization so the activity remains observable.
        constexpr int kBaseline = 2048;
        const int spikeLen = static_cast<int>(kSampleRate * 0.0016);  // 1.6 ms
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        std::normal_distribution<double> noise(
            0.0, inputUvToAdcCounts(kSpikeNoiseUv));
        for (int frame = 0; frame < kBatchFrames; ++frame) {
            const quint32 timestamp = m_timestamp & kTimestampMask;
            for (int channel = 0; channel < kChannels; ++channel) {
                double value = 0.0;
                if (m_spikeCountdown[channel] > 0) {
                    const double t = static_cast<double>(spikeLen - m_spikeCountdown[channel]) /
                                     static_cast<double>(spikeLen);
                    value = spikeShape(t) * m_spikePeakCounts[channel];
                    --m_spikeCountdown[channel];
                } else if (m_spikeRateHz[channel] > 0.0 &&
                           uni(m_rng) <
                               m_spikeRateHz[channel] / static_cast<double>(kSampleRate)) {
                    m_spikeCountdown[channel] = spikeLen;
                }
                const int adc = qBound(
                    0, kBaseline + static_cast<int>((value + noise(m_rng)) *
                                                       signalScale(channel, timestamp)), 4095);
                const quint32 raw =
                    (timestamp << kTimestampShift) |
                    static_cast<quint32>(adc);
                qToLittleEndian<quint32>(
                    raw,
                    reinterpret_cast<uchar *>(
                        output + frame * kFrameBytes + channel * kBytesPerPoint));
            }
            m_timestamp = (m_timestamp + 1u) & kTimestampMask;
        }
    } else {
        for (int frame = 0; frame < kBatchFrames; ++frame) {
            const quint32 timestamp = m_timestamp & kTimestampMask;
            for (int channel = 0; channel < kChannels; ++channel) {
                const int amplitude = 300 + (channel % 8) * 60;
                const int baseline = 2048 + (channel % 4) * 200;
                const int adc = qBound(
                    0,
                    baseline + static_cast<int>(amplitude * m_sinState[channel] *
                                                signalScale(channel, timestamp)),
                    4095);
                const quint32 raw =
                    (timestamp << kTimestampShift) |
                    static_cast<quint32>(adc);
                qToLittleEndian<quint32>(
                    raw,
                    reinterpret_cast<uchar *>(
                        output + frame * kFrameBytes + channel * kBytesPerPoint));

                const double nextSin =
                    m_sinState[channel] * m_cosStep[channel] +
                    m_cosState[channel] * m_sinStep[channel];
                const double nextCos =
                    m_cosState[channel] * m_cosStep[channel] -
                    m_sinState[channel] * m_sinStep[channel];
                m_sinState[channel] = nextSin;
                m_cosState[channel] = nextCos;
            }
            m_timestamp = (m_timestamp + 1u) & kTimestampMask;
        }
    }

    const auto clients = m_streamClients.values();
    for (QTcpSocket *socket : clients) {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }
        if (socket->bytesToWrite() <= kMaxPendingBytes) {
            socket->write(payload);
        }
    }
}

int runDummyServerMode(QCoreApplication &app, const QStringList &arguments)
{
    const int controlPort =
        argumentPort(arguments, QStringLiteral("--control-port"), 10086);
    const int dataPort =
        argumentPort(arguments, QStringLiteral("--data-port"), controlPort);

    DummyStreamServer server;
    for (const QString &arg : arguments) {
        if (arg.compare(QStringLiteral("--waveform=spike"), Qt::CaseInsensitive) == 0) {
            server.setWaveform(DummyWaveform::Spike);
        }
        if (arg.compare(QStringLiteral("--tdm"), Qt::CaseInsensitive) == 0) {
            server.setTdmSimulation(true);
        }
    }
    QString error;
    if (!server.start(controlPort, dataPort, &error)) {
        QTextStream(stderr) << "ERROR " << error << Qt::endl;
        return 2;
    }

    QTextStream(stdout) << "READY control=" << controlPort
                        << " data=" << dataPort
                        << " fs=" << DummyStreamServer::kSampleRate
                        << " ch0=" << DummyStreamServer::kBaseFrequencyHz
                        << " model=SIMULATED"
                        << " tdm=" << (server.tdmSimulation() ? "four-phase-fixture" : "off")
                        << " spi_reply=legacy-zero-placeholder"
                        << Qt::endl;
    return app.exec();
}

}  // namespace ccv2
