#pragma once

#include <QWidget>
#include <QPixmap>
#include <QVector>
#include <QColor>
#include <QMap>
#include <QString>

namespace ccv2 {

// Continuous multi-channel sweep oscilloscope, modeled on Intan-RHX
// MultiWaveformPlot: the drawn waveform persists in a "core" pixmap and each
// refresh only paints the newly-arrived samples at the advancing sweep cursor
// (wrap-around). Old pixels to the left are never re-rendered. High sample
// rates are handled with min/max-per-column decimation (vertical envelope).
class SweepWaveformView : public QWidget {
    Q_OBJECT
public:
    explicit SweepWaveformView(QWidget *parent = nullptr);

    // Layout / scaling
    void setChannels(const QVector<int> &channels);   // resets layout + sweep
    void setSampleRate(double fs);                     // samples/sec per channel
    void setTimeSpanSeconds(double seconds);           // full window width in time
    void setYFullScaleVolts(double volts);             // band full-scale, centered at mid
    void setVoltageMid(double volts);                  // band center voltage

    // Theming (keys: plotBg, grid, minorGrid, axis, wave, border)
    void setThemePalette(const QMap<QString, QString> &palette);
    void setSeriesColors(const QVector<QColor> &colors);
    void setPlaceholderText(const QString &text);

    // Feed new samples in time order. samplesByChannel[i] belongs to
    // channels()[i]; every inner vector should carry the same count.
    void appendBatch(const QVector<QVector<double>> &samplesByChannel);
    // Same, with per-sample spike detection flags (drawn as markers).
    void appendBatch(const QVector<QVector<double>> &samplesByChannel,
                     const QVector<QVector<bool>> &spikeFlags);

    void resetSweep();   // clear waveform, cursor back to left
    const QVector<int> &channels() const { return m_channels; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void recalcGeometry();
    void rebuildBackground();          // (re)build m_bgPixmap and reset m_corePixmap
    void ensurePixmaps();
    void restoreBackgroundColumns(QPainter &p, int colStart, int nCols);
    double bandTop(int idx) const;
    double bandHeight() const;
    double valueToY(int idx, double v) const;

    QVector<int> m_channels;
    QVector<QColor> m_colors;
    double m_fs = 20000.0;
    double m_timeSpan = 4.0;
    double m_yFullScale = 1.8;
    double m_yMid = 0.9;
    QString m_placeholder = QStringLiteral("等待数据…");

    // theme
    QColor m_plotBg{0x0d, 0x11, 0x17};
    QColor m_grid{0x21, 0x26, 0x2d};
    QColor m_minorGrid{0x18, 0x1d, 0x24};
    QColor m_axis{0x8b, 0x94, 0x9e};
    QColor m_wave{0x4f, 0xb6, 0xc4};
    QColor m_border{0x30, 0x36, 0x3d};
    QColor m_sweepCursor{0xf8, 0x51, 0x49};

    // pixmaps
    QPixmap m_bgPixmap;     // static grid/labels/separators
    QPixmap m_corePixmap;   // bg + persisted waveform
    bool m_pixmapsValid = false;

    // plot geometry (logical px)
    int m_marginL = 52;
    int m_marginR = 10;
    int m_marginT = 8;
    int m_marginB = 20;
    int m_plotW = 0;
    int m_plotH = 0;

    // sweep state
    int m_writeX = 0;                  // current column in [0, m_plotW)
    double m_samplesPerColumn = 1.0;
    double m_colAccum = 0.0;           // fractional sample accumulator
    QVector<double> m_colMin;
    QVector<double> m_colMax;
    QVector<bool> m_colHas;
    QVector<bool> m_colSpike;
    QColor m_spikeMark{0xff, 0xb4, 0x54};
    bool m_hasData = false;

    static constexpr int kGapColumns = 6;  // blank gap ahead of the cursor
};

}  // namespace ccv2
