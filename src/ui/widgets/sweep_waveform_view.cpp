#include "ui/widgets/sweep_waveform_view.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <algorithm>
#include <climits>
#include <cmath>

namespace ccv2 {

namespace {
QColor colorFrom(const QMap<QString, QString> &p, const QString &key, const QColor &fallback) {
    auto it = p.constFind(key);
    if (it == p.cend()) return fallback;
    QColor c(it.value());
    return c.isValid() ? c : fallback;
}
}  // namespace

SweepWaveformView::SweepWaveformView(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumHeight(120);
}

void SweepWaveformView::setChannels(const QVector<int> &channels) {
    m_channels = channels;
    const int n = m_channels.size();
    m_colMin.fill(0.0, n);
    m_colMax.fill(0.0, n);
    m_colHas.fill(false, n);
    m_colSpike.fill(false, n);
    m_pixmapsValid = false;  // force rebuild (band layout changed)
    resetSweep();
    update();
}

void SweepWaveformView::setSampleRate(double fs) {
    m_fs = std::max(1.0, fs);
    recalcGeometry();
    resetSweep();
    update();
}

void SweepWaveformView::setTimeSpanSeconds(double seconds) {
    m_timeSpan = std::max(0.05, seconds);
    m_pixmapsValid = false;  // time grid changed
    recalcGeometry();
    resetSweep();
    update();
}

void SweepWaveformView::setYFullScaleVolts(double volts) {
    m_yFullScale = std::max(1e-6, volts);
    update();
}

void SweepWaveformView::setVoltageMid(double volts) {
    m_yMid = volts;
    update();
}

void SweepWaveformView::setThemePalette(const QMap<QString, QString> &palette) {
    m_plotBg = colorFrom(palette, QStringLiteral("plotBg"), m_plotBg);
    m_grid = colorFrom(palette, QStringLiteral("grid"), m_grid);
    m_minorGrid = colorFrom(palette, QStringLiteral("minorGrid"), m_minorGrid);
    m_axis = colorFrom(palette, QStringLiteral("axis"), m_axis);
    m_wave = colorFrom(palette, QStringLiteral("wave"), m_wave);
    m_border = colorFrom(palette, QStringLiteral("border"), m_border);
    m_pixmapsValid = false;
    update();
}

void SweepWaveformView::setSeriesColors(const QVector<QColor> &colors) {
    m_colors = colors;
    update();
}

void SweepWaveformView::setPlaceholderText(const QString &text) {
    m_placeholder = text;
    update();
}

double SweepWaveformView::bandHeight() const {
    const int n = std::max(1, static_cast<int>(m_channels.size()));
    return static_cast<double>(m_plotH) / n;
}

double SweepWaveformView::bandTop(int idx) const {
    return m_marginT + idx * bandHeight();
}

double SweepWaveformView::valueToY(int idx, double v) const {
    const double bh = bandHeight();
    const double pad = bh * 0.08;
    const double usable = bh - 2.0 * pad;
    const double lo = m_yMid - m_yFullScale * 0.5;
    double frac = (v - lo) / m_yFullScale;   // 0 at bottom, 1 at top
    frac = std::clamp(frac, 0.0, 1.0);
    return bandTop(idx) + pad + (1.0 - frac) * usable;
}

void SweepWaveformView::recalcGeometry() {
    m_plotW = std::max(1, width() - m_marginL - m_marginR);
    m_plotH = std::max(1, height() - m_marginT - m_marginB);
    const double totalSamples = m_fs * m_timeSpan;
    m_samplesPerColumn = std::max(1e-6, totalSamples / static_cast<double>(m_plotW));
}

void SweepWaveformView::ensurePixmaps() {
    recalcGeometry();
    const QSize sz = size();
    if (sz.width() <= 0 || sz.height() <= 0) return;
    if (!m_pixmapsValid || m_bgPixmap.size() != sz) {
        m_bgPixmap = QPixmap(sz);
        m_corePixmap = QPixmap(sz);
        rebuildBackground();
        m_corePixmap = m_bgPixmap.copy();
        m_writeX = 0;
        m_colAccum = 0.0;
        std::fill(m_colHas.begin(), m_colHas.end(), false);
        m_pixmapsValid = true;
    }
}

void SweepWaveformView::rebuildBackground() {
    m_bgPixmap.fill(m_plotBg);
    QPainter p(&m_bgPixmap);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRect plot(m_marginL, m_marginT, m_plotW, m_plotH);
    p.fillRect(plot, m_plotBg);

    // Vertical time grid lines, one per second.
    p.setPen(QPen(m_grid, 1));
    const double pxPerSec = m_plotW / std::max(1e-6, m_timeSpan);
    for (double t = 0.0; t <= m_timeSpan + 1e-6; t += 1.0) {
        const int x = m_marginL + static_cast<int>(std::lround(t * pxPerSec));
        if (x > m_marginL + m_plotW) break;
        p.drawLine(x, m_marginT, x, m_marginT + m_plotH);
    }

    // Per-channel band: separator + faint center baseline.
    const int n = std::max(1, static_cast<int>(m_channels.size()));
    const double bh = bandHeight();
    p.setFont(QFont(QStringLiteral("Consolas"), 8));
    for (int i = 0; i < n; ++i) {
        const int top = static_cast<int>(std::lround(bandTop(i)));
        const int mid = static_cast<int>(std::lround(bandTop(i) + bh / 2.0));
        if (i > 0) {
            p.setPen(QPen(m_border, 1));
            p.drawLine(m_marginL, top, m_marginL + m_plotW, top);
        }
        p.setPen(QPen(m_minorGrid, 1, Qt::DotLine));
        p.drawLine(m_marginL, mid, m_marginL + m_plotW, mid);
        // Channel label in left margin.
        if (i < m_channels.size()) {
            p.setPen(m_axis);
            p.drawText(QRect(2, top, m_marginL - 6, static_cast<int>(bh)),
                       Qt::AlignVCenter | Qt::AlignRight,
                       QStringLiteral("CH%1").arg(m_channels[i]));
        }
    }

    // Plot border + time axis labels.
    p.setPen(QPen(m_border, 1));
    p.drawRect(plot.adjusted(0, 0, -1, -1));
    p.setPen(m_axis);
    p.setFont(QFont(QStringLiteral("Consolas"), 8));
    for (double t = 0.0; t <= m_timeSpan + 1e-6; t += 1.0) {
        const int x = m_marginL + static_cast<int>(std::lround(t * pxPerSec));
        if (x > m_marginL + m_plotW) break;
        p.drawText(QRect(x - 20, m_marginT + m_plotH + 2, 40, m_marginB - 2),
                   Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("%1s").arg(t, 0, 'f', 0));
    }
}

void SweepWaveformView::restoreBackgroundColumns(QPainter &p, int colStart, int nCols) {
    for (int k = 0; k < nCols; ++k) {
        int c = (colStart + k) % m_plotW;
        if (c < 0) c += m_plotW;
        const int x = m_marginL + c;
        p.drawPixmap(QRect(x, m_marginT, 1, m_plotH),
                     m_bgPixmap, QRect(x, m_marginT, 1, m_plotH));
    }
}

void SweepWaveformView::resetSweep() {
    if (m_pixmapsValid && !m_bgPixmap.isNull()) {
        m_corePixmap = m_bgPixmap.copy();
    }
    m_writeX = 0;
    m_colAccum = 0.0;
    std::fill(m_colHas.begin(), m_colHas.end(), false);
    m_hasData = false;
    update();
}

void SweepWaveformView::appendBatch(const QVector<QVector<double>> &samplesByChannel) {
    appendBatch(samplesByChannel, {});
}

void SweepWaveformView::appendBatch(const QVector<QVector<double>> &samplesByChannel,
                                    const QVector<QVector<bool>> &spikeFlags) {
    if (m_channels.isEmpty() || samplesByChannel.isEmpty()) return;
    ensurePixmaps();
    if (m_corePixmap.isNull()) return;

    const int nCh = m_channels.size();
    const bool hasFlags = spikeFlags.size() == nCh;
    int n = INT_MAX;
    for (int i = 0; i < nCh && i < samplesByChannel.size(); ++i) {
        n = std::min<int>(n, samplesByChannel[i].size());
    }
    if (n == INT_MAX || n <= 0) return;

    QPainter p(&m_corePixmap);
    p.setRenderHint(QPainter::Antialiasing, false);

    for (int t = 0; t < n; ++t) {
        for (int i = 0; i < nCh; ++i) {
            const double v = samplesByChannel[i][t];
            if (!std::isfinite(v)) continue;
            if (!m_colHas[i]) {
                m_colMin[i] = v;
                m_colMax[i] = v;
                m_colHas[i] = true;
            } else {
                m_colMin[i] = std::min(m_colMin[i], v);
                m_colMax[i] = std::max(m_colMax[i], v);
            }
            if (hasFlags && t < spikeFlags[i].size() && spikeFlags[i][t]) {
                m_colSpike[i] = true;
            }
        }
        m_colAccum += 1.0;

        while (m_colAccum >= m_samplesPerColumn) {
            // Clear current column + a blank gap ahead (the moving sweep gap).
            restoreBackgroundColumns(p, m_writeX, kGapColumns + 1);

            const int x = m_marginL + m_writeX;
            for (int i = 0; i < nCh; ++i) {
                if (!m_colHas[i]) continue;
                const double y1 = valueToY(i, m_colMax[i]);  // top
                const double y2 = valueToY(i, m_colMin[i]);  // bottom
                p.setPen(QPen(m_colors.value(i, m_wave), 1));
                if (std::abs(y2 - y1) < 1.0) {
                    p.drawPoint(x, static_cast<int>(std::lround(y1)));
                } else {
                    p.drawLine(QLineF(x, y1, x, y2));
                }
                if (m_colSpike[i]) {
                    const int top = static_cast<int>(std::lround(bandTop(i))) + 2;
                    p.setPen(QPen(m_spikeMark, 1));
                    p.drawLine(x, top, x, top + 4);  // small tick at band top
                    m_colSpike[i] = false;
                }
                m_colHas[i] = false;
            }

            m_writeX = (m_writeX + 1) % m_plotW;
            m_colAccum -= m_samplesPerColumn;
        }
    }

    m_hasData = true;
    update();
}

void SweepWaveformView::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);

    if (m_channels.isEmpty()) {
        painter.fillRect(rect(), m_plotBg);
        painter.setPen(m_axis);
        painter.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    ensurePixmaps();
    if (m_corePixmap.isNull()) {
        painter.fillRect(rect(), m_plotBg);
        return;
    }

    painter.drawPixmap(0, 0, m_corePixmap);

    if (!m_hasData) {
        painter.setPen(m_axis);
        painter.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    // Transient layer: sweep cursor.
    const int cx = m_marginL + m_writeX;
    painter.setPen(QPen(m_sweepCursor, 1));
    painter.drawLine(cx, m_marginT, cx, m_marginT + m_plotH);
}

void SweepWaveformView::resizeEvent(QResizeEvent *event) {
    Q_UNUSED(event);
    m_pixmapsValid = false;
    ensurePixmaps();
    update();
}

}  // namespace ccv2
