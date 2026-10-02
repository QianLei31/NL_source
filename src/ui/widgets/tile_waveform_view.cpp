#include "tile_waveform_view.h"
#include <cmath>

namespace ccv2 {

TileWaveformView::TileWaveformView(QWidget *parent)
    : QWidget(parent)
{
    m_grid = new QGridLayout(this);
    m_grid->setSpacing(4);
    m_grid->setContentsMargins(4, 4, 4, 4);
}

void TileWaveformView::setChannels(const QVector<int> &globalChannels)
{
    // Clear existing
    for (auto *w : m_widgets) {
        m_grid->removeWidget(w);
        delete w;
    }
    m_widgets.clear();
    m_channels = globalChannels;

    int n = globalChannels.size();
    int cols = qMax(1, (int)std::ceil(std::sqrt((double)n)));
    int rows = (n + cols - 1) / cols;

    for (int i = 0; i < n; ++i) {
        auto *w = new WaveformWidget(QStringLiteral("CH%1").arg(globalChannels[i]), this);
        m_grid->addWidget(w, i / cols, i % cols);
        m_widgets.append(w);
    }
    Q_UNUSED(rows);
}

void TileWaveformView::onPreviewBatch(const QVector<int> &channels, const QVector<QVector<double>> &samples)
{
    Q_UNUSED(channels);
    for (int i = 0; i < m_widgets.size() && i < samples.size(); ++i) {
        m_widgets[i]->setData(samples[i]);
    }
}

} // namespace ccv2
