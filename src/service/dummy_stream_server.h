#pragma once

#include <QObject>
#include <QSet>
#include <QVector>

#include <random>

class QCoreApplication;
class QTcpServer;
class QTcpSocket;
class QTimer;

namespace ccv2 {

enum class DummyWaveform { Sine, Spike };

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

signals:
    void serverError(const QString &message);

private:
    void acceptSinglePortClients();
    void acceptControlClients();
    void acceptDataClients();
    void removeClient(QTcpSocket *socket);
    void resetPhases();
    void sendBatch();

    QTcpServer *m_controlServer{nullptr};
    QTcpServer *m_dataServer{nullptr};
    QTimer *m_streamTimer{nullptr};
    QSet<QTcpSocket *> m_controlClients;
    QSet<QTcpSocket *> m_streamClients;
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
