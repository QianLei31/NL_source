#pragma once
#include <QWidget>
#include <QVector>
#include <QColor>
#include <QMap>
#include <QSet>

#include "src/network/data_sorter.h"
#include "src/theme/theme_manager.h"

namespace ccv2 {

/// 16x16 grid showing AC RMS/status for all 256 global channels
class ActivityMapView : public QWidget {
    Q_OBJECT
public:
    enum MetricMode {
        MetricRms = 0,
        MetricP2p = 1,
        MetricMean = 2,
        MetricSaturation = 3,
    };

    explicit ActivityMapView(QWidget *parent = nullptr);

    void setPalette(const ThemePalette &palette);
    void setPaletteColors(const QMap<QString, QString> &palette);
    void setSelectedChannels(const QVector<int> &channels);
    void setMetricMode(int mode);
    void setTdmDisplay(bool enabled, bool pair02);
    void clearMetrics();
    double maxRms() const;

signals:
    void channelClicked(int globalChannel);

public slots:
    void onChannelMetrics(int globalChannel, double rms, double p2p, bool saturated, bool packetLoss);
    void setMetrics(const QVector<RealtimeChannelMetric> &metrics);
    void setTdmMetrics(const QVector<RealtimeChannelMetric> &metrics);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    struct ChannelState {
        double mean = 0.0;
        double rms = 0.0;
        double p2p = 0.0;
        bool saturated = false;
        bool packetLoss = false;
        bool valid = false;
    };
    QColor colorFromPalette(const QString &key, const QColor &fallback) const;
    QColor activityColor(double intensity) const;
    double metricValue(const ChannelState &state) const;
    double metricMax() const;
    QString metricName() const;
    QString metricScaleText() const;
    QColor colorForState(const ChannelState &state,
                         const QColor &warning,
                         const QColor &error) const;
    QRectF mapRect() const;
    int channelAt(const QPointF &point) const;

    QVector<ChannelState> m_states;  // size = 256
    QVector<ChannelState> m_tdmStates;  // size = 1024, channel * 4 + physical phase
    QSet<int> m_selectedChannels;
    ThemePalette m_palette;
    QMap<QString, QString> m_paletteColors;
    double m_maxRms = 1.0;
    double m_maxP2p = 1.0;
    double m_tdmMaxRms = 1.0;
    double m_tdmMaxP2p = 1.0;
    int m_metricMode{MetricRms};
    bool m_tdmDisplay{false};
    bool m_pair02{true};
};

} // namespace ccv2
