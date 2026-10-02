#pragma once

#include <QColor>
#include <QMap>
#include <QPixmap>
#include <QString>
#include <QVector>
#include <QWidget>

namespace ccv2 {

class SpikeSnippetStore;

// Blackrock-Central-style spike panel: one small cell per electrode, each
// overlaying the last N threshold-aligned waveforms so unit activity across
// the whole array is visible at a glance.
//
// Traces live in a persistent layer that is only ever drawn into
// incrementally (new snippets) and optionally faded, so a 512-cell grid stays
// cheap no matter how many waveforms are retained.
class SpikeGridView : public QWidget {
    Q_OBJECT
public:
    explicit SpikeGridView(QWidget *parent = nullptr);

    void setStore(SpikeSnippetStore *store);
    // Electrode label per lane (raw channel numbers, or ELE numbers in TDM).
    void setLaneLabels(const QVector<int> &labels);
    void setColumns(int columns);            // 0 = auto (near-square)
    void setYFullScaleMicrovolts(double uv);  // peak-to-peak window per cell
    void setAutoScale(bool on);
    void setFadeEnabled(bool on);
    // Bounds GUI work under low thresholds/high event rates. Excess display
    // snippets are skipped; detector totals and retained data remain intact.
    void setSnippetDrawLimits(int incrementalPerLane, int rebuildPerLane);
    void setSelectedLane(int lane);
    int selectedLane() const { return m_selected; }
    // Single-lane mode: one cell fills the widget, with time/amplitude axes.
    // A second view bound to the same store keeps its own read watermark, so
    // the grid and the detail view never steal snippets from each other.
    void setFocusLane(int lane);

    // Pull whatever arrived since the last call and draw it. Cheap: only the
    // new snippets are rendered.
    void pullNewSnippets();
    // Re-render every retained snippet (geometry/scale changes).
    void rebuildAll();
    void clearTraces();

    void setThemePalette(const QMap<QString, QString> &palette);
    void setPlaceholderText(const QString &text);
    // Used by the detail view's time axis.
    void setTimebase(double sampleRate, int preSamples);
    // Spike counts for the per-cell rate readout; caller supplies the rate.
    void setRates(const QVector<double> &ratesHz);
    void setThresholds(const QVector<double> &thresholdsV);
    // In focused single-lane mode the threshold line can be dragged
    // vertically. Array-grid selection behavior is unchanged.
    void setThresholdEditingEnabled(bool enabled);

signals:
    void laneClicked(int lane);
    void thresholdDragged(double thresholdV);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    struct CellRect {
        int x = 0, y = 0, w = 0, h = 0;
    };

    void recalcGeometry();
    void rebuildBackground();
    void ensureLayers();
    CellRect cellRect(int lane) const;
    void drawSnippets(QPainter &p, int lane, const QVector<float> &flat, int count);
    double laneScale(int lane) const;
    void updateDraggedThreshold(const QPoint &position);

    SpikeSnippetStore *m_store = nullptr;
    QVector<int> m_labels;
    QVector<quint64> m_seen;       // per-lane sequence watermark
    QVector<double> m_rates;
    QVector<double> m_thresholds;
    QVector<double> m_autoScaleV;  // per-lane adaptive full scale (volts)

    int m_columns = 0;
    int m_effColumns = 16;
    int m_rows = 16;
    double m_yFullScaleV = 200e-6;
    bool m_autoScale = false;
    bool m_fade = true;
    bool m_thresholdEditingEnabled = false;
    bool m_draggingThreshold = false;
    int m_selected = -1;
    // -2 = array grid, -1 = empty detail view, >=0 = focused detail lane.
    int m_focusLane = -2;
    int m_incrementalDrawLimit = 6;
    int m_rebuildDrawLimit = 20;
    double m_sampleRate = 20000.0;
    int m_preSamples = 8;
    QString m_placeholder = QStringLiteral("等待数据…");

    QColor m_plotBg{0x0d, 0x11, 0x17};
    QColor m_grid{0x21, 0x26, 0x2d};
    QColor m_axis{0x8b, 0x94, 0x9e};
    QColor m_wave{0x4f, 0xb6, 0xc4};
    QColor m_border{0x30, 0x36, 0x3d};
    QColor m_threshColor{0xd8, 0xa2, 0x3b};
    QColor m_selectColor{0x4d, 0x8f, 0xe8};

    QPixmap m_bgPixmap;
    QPixmap m_traceLayer;  // ARGB, faded in place
    bool m_layersValid = false;
    int m_marginL = 4;
    int m_marginT = 4;
    int m_marginR = 4;
    int m_marginB = 4;
    int m_cellW = 0;
    int m_cellH = 0;
    int m_fadeTick = 0;
};

}  // namespace ccv2
