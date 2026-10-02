#pragma once
#include <QObject>
#include <QVector>
#include <QByteArray>
#include <atomic>

namespace ccv2 {

class AnalyzerPipeline : public QObject {
    Q_OBJECT
public:
    explicit AnalyzerPipeline(QObject *parent = nullptr);

    void setChannels(const QVector<int> &globalChannels);
    void setRingCapacity(int samplesPerChannel);

    void startRecording(const QString &baseDir, const QString &sessionName);
    void stopRecording();
    bool isRecording() const { return m_recording.load(); }

    qint64 ringBufferTotalWritten(int globalChannel) const;

public slots:
    void onRawChunk(const QByteArray &chunk);
    void requestCapture(int points, const QString &windowFn);

signals:
    void captureReady(int channel, const QVector<double> &timeSeries,
                      const QVector<double> &spectrum, const QVariantMap &metrics);
    void recordingStateChanged(bool recording, const QString &path);

private:
    QVector<int> m_channels;
    int m_ringCapacity = 131072;  // 1 << 17 minimum per requirement
    std::atomic_bool m_recording{false};
    // Ring buffers and FFT worker will be fully implemented in Phase 4
    QVector<qint64> m_totalWritten;
};

} // namespace ccv2
