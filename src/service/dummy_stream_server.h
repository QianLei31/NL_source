#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QVector>

#include <array>
#include <optional>
#include <random>

class QCoreApplication;
class QTcpServer;
class QTcpSocket;
class QTimer;

namespace ccv2 {

enum class DummyWaveform { Sine, Spike };

// Local SIMULATED state, never a claim of board readback. Access from the
// server's QObject thread. Unknown power-on global states remain nullopt.
struct DummyProtocolState {
    std::array<quint16, 256> rec{};
    std::array<quint16, 256> dac{};
    std::array<quint16, 256> dacCt{};
    std::optional<bool> analogResetAsserted;
    std::optional<bool> globalDacEnabled;
    std::optional<bool> cbokLow;
    quint64 spiCommands{0};
    quint64 supportedCommands{0};
    quint64 unsupportedCommands{0};
    quint64 malformedCommands{0};
    quint32 lastSpiWord{0};
    bool lastCommandSupported{false};
    QString lastMessage;
};

class DummyStreamServer : public QObject {
    Q_OBJECT

public:
    static constexpr int kChannels = 256;
    static constexpr int kSampleRate = 20000;
    static constexpr double kBaseFrequencyHz = 177.0;
    static constexpr double kFrequencyStepHz = 0.5;

    explicit DummyStreamServer(QObject *parent = nullptr);
    ~DummyStreamServer() override;
    bool start(int controlPort, int dataPort, QString *error = nullptr);

    void setWaveform(DummyWaveform waveform) { m_waveform = waveform; }
    DummyWaveform waveform() const { return m_waveform; }

    DummyProtocolState protocolState() const { return m_protocol; }
    double channelGain(int channel) const;
    bool channelSignalEnabled(int channel) const;
    // Explicit synthetic routing fixture, not an undocumented board command.
    // Four phases have AC amplitude factors 1, 1.25, 1.5, 1.75.
    void setTdmSimulation(bool enabled) { m_tdmSimulation = enabled; }
    bool tdmSimulation() const { return m_tdmSimulation; }
    int simulatedElectrode(int adcChannel, quint32 sourceFrame) const;

signals:
    void serverError(const QString &message);
    void protocolCommandProcessed(quint32 word, bool supported);

private:
    void acceptSinglePortClients();
    void acceptControlClients();
    void acceptDataClients();
    void removeClient(QTcpSocket *socket);
    void readControlCommands(QTcpSocket *socket);
    void applySpiWord(quint32 word);
    void rejectMalformed(QTcpSocket *socket, const QString &reason);
    double signalScale(int channel, quint32 sourceFrame) const;
    void resetPhases();
    void sendBatch();

    QTcpServer *m_controlServer{nullptr};
    QTcpServer *m_dataServer{nullptr};
    QTimer *m_streamTimer{nullptr};
    QSet<QTcpSocket *> m_controlClients;
    QSet<QTcpSocket *> m_streamClients;
    QHash<QTcpSocket *, QByteArray> m_commandBuffers;
    DummyProtocolState m_protocol;
    bool m_tdmSimulation{false};
    QVector<double> m_sinState;
    QVector<double> m_cosState;
    QVector<double> m_sinStep;
    QVector<double> m_cosStep;
    DummyWaveform m_waveform{DummyWaveform::Sine};
    QVector<int> m_spikeCountdown;   // samples remaining in current spike, per channel
    QVector<double> m_spikeRateHz;   // fixed firing-rate profile; zero means silent
    QVector<double> m_spikePeakCounts;  // fixed input-referred amplitude after gain
    quint32 m_timestamp{0};
    std::mt19937 m_rng{0xC0FFEEu};
    bool m_singlePort{true};
    bool m_dualPortArmed{false};
};

int runDummyServerMode(QCoreApplication &app, const QStringList &arguments);

}  // namespace ccv2
