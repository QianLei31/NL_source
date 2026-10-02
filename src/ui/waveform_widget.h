#pragma once

#include <QColor>
#include <QMap>
#include <QPair>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

namespace ccv2 {

class WaveformWidget : public QWidget {
    Q_OBJECT

public:
    struct PlotSeries {
        QVector<double> data;
        QVector<double> xData;
        QColor color;
        QString label;
    };

    explicit WaveformWidget(const QString &title = QString(), QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setData(const QVector<double> &data);
    void setSeries(const QVector<PlotSeries> &series);
    void setYRange(double yMin, double yMax);
    void setXRange(double xMin, double xMax);
    void setAxisLabels(const QString &xLabel, const QString &yLabel);
    void setMaxRenderPoints(int points);
    void setPaletteColors(const QMap<QString, QString> &palette);
    void setDenseGrid(bool enabled);
    void setLogXScale(bool enabled);
    // Treat Y data as log10(value in SI base units): tick labels render the
    // underlying value in engineering notation (e.g. -7.5 -> "31.6n").
    void setYLog10Labels(bool enabled);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QVector<double> downsample(const QVector<double> &data) const;
    PlotSeries downsampleSeries(const PlotSeries &series) const;
    QRect plotRectForRange(double yMin, double yMax) const;
    // The X range currently on screen (zoom-aware; clamped to positive values
    // in log mode). paintEvent and the box-zoom mapping must share this.
    QPair<double, double> displayedXRange() const;
    void resetZoom();

    QString m_title;
    QString m_xLabel{QStringLiteral("样本")};
    QString m_yLabel{QStringLiteral("数值")};
    QVector<double> m_data;
    QVector<PlotSeries> m_series;
    double m_yMin{0.0};
    double m_yMax{1.8};
    double m_xMin{0.0};
    double m_xMax{1.0};
    bool m_hasXRange{false};
    bool m_logXScale{false};
    bool m_yLog10Labels{false};
    bool m_zoomActive{false};
    double m_zoomXMin{0.0};
    double m_zoomXMax{1.0};
    double m_zoomYMin{0.0};
    double m_zoomYMax{1.8};
    bool m_draggingZoom{false};
    QPoint m_zoomDragStart;
    QPoint m_zoomDragCurrent;
    int m_maxRenderPoints{1600};
    bool m_denseGrid{true};
    QMap<QString, QString> m_palette;
};

}  // namespace ccv2
