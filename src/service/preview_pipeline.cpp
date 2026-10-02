#include "preview_pipeline.h"
#include "src/core/spi_protocol.h"
#include <cmath>

namespace ccv2 {

PreviewPipeline::PreviewPipeline(QObject *parent)
    : QObject(parent)
{
    m_emitTimer = new QTimer(this);
    m_emitTimer->setTimerType(Qt::PreciseTimer);
    connect(m_emitTimer, &QTimer::timeout, this, &PreviewPipeline::emitBatch);
}

void PreviewPipeline::setSelectedChannels(const QVector<int> &globalChannels)
{
    m_channels = globalChannels;
    m_decimatedBuffer.resize(m_channels.size());
    for (auto &buf : m_decimatedBuffer)
        buf.clear();
}

void PreviewPipeline::setTargetUiHz(int hz)
{
    m_targetUiHz = qBound(1, hz, 60);
    if (m_emitTimer->isActive()) {
        m_emitTimer->setInterval(1000 / m_targetUiHz);
    }
}

void PreviewPipeline::setPointsPerChannel(int pts)
{
    m_pointsPerChannel = qBound(100, pts, 10000);
}

void PreviewPipeline::onRawChunk(const QByteArray &chunk)
{
    // Parse frames from raw chunk
    // Each frame = 256 channels × 4 bytes (int32)
    const int frameBytes = kFrameBytes;
    const char *data = chunk.constData();
    const int totalBytes = chunk.size();

    for (int offset = 0; offset + frameBytes <= totalBytes; offset += frameBytes) {
        // Decimation: only process every m_decimationFactor-th frame
        static int frameCounter = 0;
        frameCounter++;
        if (m_decimationFactor > 1 && (frameCounter % m_decimationFactor) != 0)
            continue;

        const qint32 *samples = reinterpret_cast<const qint32 *>(data + offset);

        for (int i = 0; i < m_channels.size(); ++i) {
            int gch = m_channels[i];
            if (gch >= 0 && gch < kChannelsPerFrame) {
                // low 12 bits = ADC sample; high 20 bits = frame timestamp -> strip it
                const quint32 adc = static_cast<quint32>(samples[gch]) & 0x0FFFu;
                double value = static_cast<double>(adc) / 4096.0 * 1.8;
                m_decimatedBuffer[i].append(value);
                // Keep buffer bounded
                while (m_decimatedBuffer[i].size() > m_pointsPerChannel)
                    m_decimatedBuffer[i].removeFirst();
            }
        }
    }

    // Start emit timer if not running
    if (!m_emitTimer->isActive() && !m_channels.isEmpty()) {
        m_emitTimer->start(1000 / m_targetUiHz);
    }
}

void PreviewPipeline::emitBatch()
{
    if (m_channels.isEmpty()) return;
    emit previewBatch(m_channels, m_decimatedBuffer);
}

} // namespace ccv2
