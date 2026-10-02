#include "analyzer_pipeline.h"
#include "src/core/spi_protocol.h"

namespace ccv2 {

AnalyzerPipeline::AnalyzerPipeline(QObject *parent)
    : QObject(parent)
{
}

void AnalyzerPipeline::setChannels(const QVector<int> &globalChannels)
{
    m_channels = globalChannels;
    m_totalWritten.resize(kChannelsPerFrame);
    m_totalWritten.fill(0);
}

void AnalyzerPipeline::setRingCapacity(int samplesPerChannel)
{
    m_ringCapacity = qMax(131072, samplesPerChannel);
}

void AnalyzerPipeline::startRecording(const QString &baseDir, const QString &sessionName)
{
    Q_UNUSED(baseDir);
    Q_UNUSED(sessionName);
    m_recording.store(true);
    emit recordingStateChanged(true, baseDir + "/" + sessionName);
}

void AnalyzerPipeline::stopRecording()
{
    m_recording.store(false);
    emit recordingStateChanged(false, QString());
}

qint64 AnalyzerPipeline::ringBufferTotalWritten(int globalChannel) const
{
    if (globalChannel < 0 || globalChannel >= m_totalWritten.size()) return 0;
    return m_totalWritten[globalChannel];
}

void AnalyzerPipeline::onRawChunk(const QByteArray &chunk)
{
    // Parse frames and write to ring buffer
    const int frameBytes = kFrameBytes;
    const char *data = chunk.constData();
    const int totalBytes = chunk.size();

    for (int offset = 0; offset + frameBytes <= totalBytes; offset += frameBytes) {
        const qint32 *samples = reinterpret_cast<const qint32 *>(data + offset);
        for (int gch : m_channels) {
            if (gch >= 0 && gch < kChannelsPerFrame) {
                // TODO: write to actual ring buffer (Phase 4)
                m_totalWritten[gch]++;
            }
        }
    }
}

void AnalyzerPipeline::requestCapture(int points, const QString &windowFn)
{
    Q_UNUSED(points);
    Q_UNUSED(windowFn);
    // TODO: snapshot ring buffer + async FFT (Phase 4)
}

} // namespace ccv2
