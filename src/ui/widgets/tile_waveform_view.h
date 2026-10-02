#pragma once
#include <QWidget>
#include <QGridLayout>
#include <QVector>
#include "src/ui/waveform_widget.h"

namespace ccv2 {

/// Grid of individual WaveformWidget tiles for small channel counts
class TileWaveformView : public QWidget {
    Q_OBJECT
public:
    explicit TileWaveformView(QWidget *parent = nullptr);

    void setChannels(const QVector<int> &globalChannels);

public slots:
    void onPreviewBatch(const QVector<int> &channels, const QVector<QVector<double>> &samples);

private:
    QGridLayout *m_grid = nullptr;
    QVector<WaveformWidget*> m_widgets;
    QVector<int> m_channels;
};

} // namespace ccv2
