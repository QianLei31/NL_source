#pragma once
#include <QWidget>
#include <QVector>
#include <QColor>
#include <QMap>
#include <QString>

namespace ccv2 {

/// Multi-channel stacked waveform view
class StackWaveformView : public QWidget {
    Q_OBJECT
public:
    enum YScaleMode {
        FixedYScale = 0,
        AdaptiveYScale = 1,
    };

    struct Trace {
        QVector<double> data;
        QString label;
        QColor color;
    };

    explicit StackWaveformView(QWidget *parent = nullptr);

    void setChannelCount(int n);
    void setPalette(const QVector<QColor> &colors);
    void setThemePalette(const QMap<QString, QString> &palette);
    void setPlaceholderText(const QString &text);
    void setTimeSpanSeconds(double seconds);
    void setTraces(const QVector<Trace> &traces);
    void setYScaleMode(YScaleMode mode);
    void setFixedYRange(double minimum, double maximum);

public slots:
    void onPreviewBatch(const QVector<int> &channels, const QVector<QVector<double>> &samples);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor colorFromTheme(const QString &key, const QColor &fallback) const;

    QVector<Trace> m_traces;
    QVector<QVector<double>> m_data;
    QVector<QColor> m_colors;
    int m_channelCount = 0;
    QString m_placeholder;
    QMap<QString, QString> m_theme;
    double m_timeSpanSeconds{0.0};
    YScaleMode m_yScaleMode{FixedYScale};
    double m_fixedYMin{0.0};
    double m_fixedYMax{1.8};
};

} // namespace ccv2
