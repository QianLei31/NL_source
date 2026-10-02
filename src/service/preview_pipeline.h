#pragma once
#include <QObject>
#include <QTimer>
#include <QVector>
#include <QByteArray>

namespace ccv2 {

class PreviewPipeline : public QObject {
    Q_OBJECT
public:
    explicit PreviewPipeline(QObject *parent = nullptr);

    void setSelectedChannels(const QVector<int> &globalChannels);
    void setTargetUiHz(int hz);
    void setPointsPerChannel(int pts);

public slots:
    void onRawChunk(const QByteArray &chunk);

signals:
    void previewBatch(const QVector<int> &channels, const QVector<QVector<double>> &samples);
    void channelMetrics(int globalChannel, double rms, double p2p, bool saturated, bool packetLoss);

private:
    void emitBatch();

    QVector<int> m_channels;
    int m_targetUiHz = 10;
    int m_pointsPerChannel = 1200;
    QTimer *m_emitTimer = nullptr;
    // Decimated buffers will be populated in Phase 3
    QVector<QVector<double>> m_decimatedBuffer;
    int m_decimationFactor = 1;
};

} // namespace ccv2
