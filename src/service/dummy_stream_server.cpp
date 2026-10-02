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

bool isStreamCommand(const QByteArray &command)
{
    return command.trimmed().toLower().startsWith("ctre");
}

bool isStopCommand(const QByteArray &command)
{
    return command.trimmed().toLower().startsWith("stop");
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
    while (m_controlServer->hasPendingConnections()) {
        QTcpSocket *socket = m_controlServer->nextPendingConnection();
        m_controlClients.insert(socket);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
            const QByteArray command = socket->readAll();
            if (isStreamCommand(command)) {
                const bool firstStream = m_streamClients.isEmpty();
                m_controlClients.remove(socket);
                m_streamClients.insert(socket);
                if (firstStream) {
                    resetPhases();
                }
            } else if (isStopCommand(command)) {
                socket->disconnectFromHost();
            } else {
                socket->write(QByteArray(12, '\0'));
            }
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                [this, socket]() { removeClient(socket); });
    }
}

void DummyStreamServer::acceptControlClients()
{
    while (m_controlServer->hasPendingConnections()) {
        QTcpSocket *socket = m_controlServer->nextPendingConnection();
        m_controlClients.insert(socket);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
            const QByteArray command = socket->readAll();
            if (isStreamCommand(command)) {
                m_dualPortArmed = true;
                resetPhases();
            } else if (isStopCommand(command)) {
                m_dualPortArmed = false;
            } else {
                socket->write(QByteArray(12, '\0'));
            }
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                [this, socket]() { removeClient(socket); });
    }
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
                    0, kBaseline + static_cast<int>(value + noise(m_rng)), 4095);
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
                    baseline + static_cast<int>(amplitude * m_sinState[channel]),
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
                        << Qt::endl;
    return app.exec();
}

}  // namespace ccv2
