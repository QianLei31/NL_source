#include "ui/widgets/spike_grid_view.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

#include "signal/spike_snippet_store.h"

namespace ccv2 {

namespace {
constexpr int kMinCellW = 26;
constexpr int kMinCellH = 20;
// Labels/rates are unreadable below this cell size; the grid then shows
// waveforms only (still the point of the panel at 512 electrodes).
constexpr int kLabelMinW = 44;
constexpr int kLabelMinH = 30;
constexpr int kDetailAxisLeft = 82;
}  // namespace

SpikeGridView::SpikeGridView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(480, 320);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
}

void SpikeGridView::setStore(SpikeSnippetStore *store) {
    m_store = store;
    const int lanes = store ? store->channels() : 0;
    m_seen.fill(0, lanes);
    m_autoScaleV.fill(0.0, lanes);
    if (m_labels.size() != lanes) {
        m_labels.resize(lanes);
        for (int i = 0; i < lanes; ++i) m_labels[i] = i;
    }
    m_layersValid = false;
    recalcGeometry();
    update();
}

void SpikeGridView::setLaneLabels(const QVector<int> &labels) {
    m_labels = labels;
    m_layersValid = false;
    update();
}

void SpikeGridView::setColumns(int columns) {
    if (m_columns == columns) return;
    m_columns = columns;
    m_layersValid = false;
    recalcGeometry();
    rebuildAll();
}

void SpikeGridView::setYFullScaleMicrovolts(double uv) {
    const double v = qMax(1e-6, uv * 1e-6);
    if (qFuzzyCompare(v, m_yFullScaleV)) return;
    m_yFullScaleV = v;
    rebuildAll();
}

void SpikeGridView::setAutoScale(bool on) {
    if (m_autoScale == on) return;
    m_autoScale = on;
    rebuildAll();
}

void SpikeGridView::setFadeEnabled(bool on) { m_fade = on; }

void SpikeGridView::setSnippetDrawLimits(int incrementalPerLane, int rebuildPerLane) {
    const int incremental = qMax(1, incrementalPerLane);
    const int rebuild = qMax(1, rebuildPerLane);
    if (m_incrementalDrawLimit == incremental &&
        m_rebuildDrawLimit == rebuild) {
        return;
    }
    m_incrementalDrawLimit = incremental;
    m_rebuildDrawLimit = rebuild;
    rebuildAll();
}

void SpikeGridView::setFocusLane(int lane) {
    if (m_focusLane == lane) return;
    m_focusLane = lane;
    setCursor(m_thresholdEditingEnabled && lane >= 0 ? Qt::SizeVerCursor
                                                     : Qt::ArrowCursor);
    m_layersValid = false;
    rebuildAll();
}

void SpikeGridView::setTimebase(double sampleRate, int preSamples) {
    m_sampleRate = qMax(1.0, sampleRate);
    m_preSamples = qMax(0, preSamples);
    update();
}

void SpikeGridView::setSelectedLane(int lane) {
    if (m_selected == lane) return;
    m_selected = lane;
    update();
}

void SpikeGridView::setPlaceholderText(const QString &text) {
    m_placeholder = text;
    update();
}

void SpikeGridView::setRates(const QVector<double> &ratesHz) {
    m_rates = ratesHz;
    update();
}

void SpikeGridView::setThresholds(const QVector<double> &thresholdsV) {
    m_thresholds = thresholdsV;
    update();
}

void SpikeGridView::setThresholdEditingEnabled(bool enabled) {
    m_thresholdEditingEnabled = enabled;
    if (!enabled) m_draggingThreshold = false;
    setCursor(enabled && m_focusLane >= 0 ? Qt::SizeVerCursor : Qt::ArrowCursor);
}

void SpikeGridView::setThemePalette(const QMap<QString, QString> &palette) {
    auto pick = [&](const QString &key, QColor &dst) {
        const QString value = palette.value(key).trimmed();
        if (!value.isEmpty()) {
            const QColor c(value);
            if (c.isValid()) dst = c;
        }
    };
    pick(QStringLiteral("plotBg"), m_plotBg);
    pick(QStringLiteral("grid"), m_grid);
    pick(QStringLiteral("axis"), m_axis);
    pick(QStringLiteral("wave"), m_wave);
    pick(QStringLiteral("border"), m_border);
    m_layersValid = false;
    // Recreating the backing pixmaps clears the trace layer. Repaint retained
    // snippets too: a paused stream has no fresh events to restore them later.
    rebuildAll();
}

void SpikeGridView::recalcGeometry() {
    const int lanes = m_store ? m_store->channels() : 0;
    if (lanes <= 0) {
        m_effColumns = 1;
        m_rows = 1;
        m_cellW = m_cellH = 0;
        return;
    }
    if (m_focusLane >= -1) {
        // Detail mode: leave room for the axes around one big cell.
        m_effColumns = 1;
        m_rows = 1;
        m_cellW = qMax(1, width() - kDetailAxisLeft - 8);
        m_cellH = qMax(1, height() - 6 - 18);
        return;
    }
    const int availW = qMax(1, width() - m_marginL - m_marginR);
    const int availH = qMax(1, height() - m_marginT - m_marginB);

    if (m_columns > 0) {
        m_effColumns = qBound(1, m_columns, lanes);
    } else {
        // Pick the column count whose cells come closest to a 4:3 shape.
        int best = 1;
        double bestScore = 1e18;
        for (int cols = 1; cols <= lanes; ++cols) {
            const int rows = (lanes + cols - 1) / cols;
            const double cw = static_cast<double>(availW) / cols;
            const double chh = static_cast<double>(availH) / rows;
            if (cw < kMinCellW || chh < kMinCellH) continue;
            const double score = std::abs(cw / qMax(1.0, chh) - 1.35);
            if (score < bestScore) {
                bestScore = score;
                best = cols;
            }
        }
        if (bestScore > 1e17) {
            // Nothing fits the minimum cell size; fall back to near-square.
            best = qMax(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(lanes)))));
        }
        m_effColumns = best;
    }
    m_rows = (lanes + m_effColumns - 1) / m_effColumns;
    m_cellW = availW / m_effColumns;
    m_cellH = availH / qMax(1, m_rows);
}

SpikeGridView::CellRect SpikeGridView::cellRect(int lane) const {
    CellRect r;
    if (m_effColumns <= 0 || m_cellW <= 0 || m_cellH <= 0) return r;
    if (m_focusLane >= -1) {
        // Only the focused lane occupies the (single) cell; every other lane
        // gets a zero-size rect so drawing/painting skips it.
        if (lane != m_focusLane) return r;
        r.x = kDetailAxisLeft;
        r.y = 6;
        r.w = m_cellW;
        r.h = m_cellH;
        return r;
    }
    const int col = lane % m_effColumns;
    const int row = lane / m_effColumns;
    r.x = m_marginL + col * m_cellW;
    r.y = m_marginT + row * m_cellH;
    r.w = m_cellW;
    r.h = m_cellH;
    return r;
}

void SpikeGridView::ensureLayers() {
    const QSize target = size() * devicePixelRatioF();
    if (m_layersValid && m_bgPixmap.size() == target) return;
    if (width() <= 0 || height() <= 0) return;

    recalcGeometry();
    m_bgPixmap = QPixmap(target);
    m_bgPixmap.setDevicePixelRatio(devicePixelRatioF());
    m_traceLayer = QPixmap(target);
    m_traceLayer.setDevicePixelRatio(devicePixelRatioF());
    m_traceLayer.fill(Qt::transparent);
    rebuildBackground();
    m_layersValid = true;
}

void SpikeGridView::rebuildBackground() {
    if (m_bgPixmap.isNull()) return;
    QPainter p(&m_bgPixmap);
    p.fillRect(rect(), m_plotBg);
    const int lanes = m_store ? m_store->channels() : 0;
    if (lanes <= 0) return;

    p.setRenderHint(QPainter::Antialiasing, false);

    if (m_focusLane >= 0) {
        const CellRect r = cellRect(m_focusLane);
        if (r.w <= 0 || r.h <= 0) return;
        QFont df = p.font();
        df.setPixelSize(10);
        p.setFont(df);
        p.setPen(QPen(m_border, 1));
        p.drawRect(r.x, r.y, r.w - 1, r.h - 1);

        // Amplitude gridlines at +/-50% and the zero baseline.
        const double half = qMax(1e-9, laneScale(m_focusLane) * 0.5);
        for (double frac : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            const double y = r.y + r.h * 0.5 - frac * (r.h * 0.5);
            p.setPen(QPen(frac == 0.0 ? m_axis : m_grid, 1,
                          frac == 0.0 ? Qt::SolidLine : Qt::DotLine));
            p.drawLine(QPointF(r.x, y), QPointF(r.x + r.w - 1, y));
            p.setPen(m_axis);
            p.drawText(QRect(0, static_cast<int>(y) - 7, r.x - 4, 14),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(frac * half * 1e6, 'f', 0) + QStringLiteral("µV"));
        }
        // Time axis, zero at the threshold crossing.
        const int len = m_store->snippetLength();
        const double msPerSample = 1000.0 / m_sampleRate;
        for (int k = 0; k < len; k += qMax(1, len / 8)) {
            const double x = r.x + static_cast<double>(k) / qMax(1, len - 1) * (r.w - 1);
            p.setPen(QPen(m_grid, 1, Qt::DotLine));
            p.drawLine(QPointF(x, r.y), QPointF(x, r.y + r.h - 1));
            p.setPen(m_axis);
            p.drawText(QRect(static_cast<int>(x) - 22, r.y + r.h + 2, 44, 14),
                       Qt::AlignCenter,
                       QString::number((k - m_preSamples) * msPerSample, 'f', 1));
        }
        p.setPen(m_axis);
        p.drawText(QRect(r.x, r.y + r.h + 2, r.w - 4, 14),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("ms"));
        return;
    }

    const bool showLabels = m_cellW >= kLabelMinW && m_cellH >= kLabelMinH;
    QFont f = p.font();
    f.setPixelSize(qBound(8, m_cellH / 5, 11));
    p.setFont(f);

    for (int lane = 0; lane < lanes; ++lane) {
        const CellRect r = cellRect(lane);
        if (r.w <= 0 || r.h <= 0) continue;
        // Cell frame + zero line: the waveform baseline sits at mid-height.
        p.setPen(QPen(m_border, 1));
        p.drawRect(r.x, r.y, r.w - 1, r.h - 1);
        p.setPen(QPen(m_grid, 1, Qt::DotLine));
        const int midY = r.y + r.h / 2;
        p.drawLine(r.x + 1, midY, r.x + r.w - 2, midY);
        if (showLabels) {
            p.setPen(m_axis);
            const int label = lane < m_labels.size() ? m_labels[lane] : lane;
            p.drawText(r.x + 3, r.y + f.pixelSize() + 1, QString::number(label));
        }
    }
}

double SpikeGridView::laneScale(int lane) const {
    if (m_autoScale && lane >= 0 && lane < m_autoScaleV.size() && m_autoScaleV[lane] > 0.0) {
        return m_autoScaleV[lane];
    }
    return m_yFullScaleV;
}

void SpikeGridView::drawSnippets(QPainter &p, int lane, const QVector<float> &flat, int count) {
    if (count <= 0 || !m_store) return;
    const int len = m_store->snippetLength();
    if (len <= 1) return;
    const CellRect r = cellRect(lane);
    if (r.w <= 2 || r.h <= 2) return;

    if (m_autoScale) {
        // Track the largest excursion seen so the cell keeps the unit in view;
        // decays slowly via rebuildAll() when the scale is recomputed.
        double peak = lane < m_autoScaleV.size() ? m_autoScaleV[lane] : 0.0;
        for (float v : flat) {
            peak = qMax(peak, std::abs(static_cast<double>(v)) * 2.4);
        }
        if (lane < m_autoScaleV.size()) m_autoScaleV[lane] = peak;
    }

    const double scale = laneScale(lane);
    const double half = qMax(1e-9, scale * 0.5);
    const int plotX = r.x + 1;
    const int plotY = r.y + 1;
    const int plotW = r.w - 2;
    const int plotH = r.h - 2;
    const double midY = plotY + plotH * 0.5;
    const double dx = static_cast<double>(plotW) / static_cast<double>(len - 1);

    p.save();
    p.setClipRect(plotX, plotY, plotW, plotH);
    p.setRenderHint(QPainter::Antialiasing, plotW > 40);
    QColor traceColor = m_wave;
    traceColor.setAlpha(plotW > 40 ? 150 : 200);
    p.setPen(QPen(traceColor, 1.0));

    QVector<QPointF> pts(len);
    for (int s = 0; s < count; ++s) {
        const float *snip = flat.constData() + static_cast<qsizetype>(s) * len;
        for (int k = 0; k < len; ++k) {
            const double y = midY - (static_cast<double>(snip[k]) / half) * (plotH * 0.5);
            pts[k] = QPointF(plotX + k * dx,
                             qBound<double>(plotY - 2.0, y, plotY + plotH + 2.0));
        }
        p.drawPolyline(pts.constData(), len);
    }
    p.restore();
}

void SpikeGridView::pullNewSnippets() {
    if (!m_store) return;
    if (m_focusLane == -1) return;
    ensureLayers();
    if (m_traceLayer.isNull()) return;

    const int lanes = m_store->channels();
    if (m_seen.size() != lanes) m_seen.fill(0, lanes);
    if (m_autoScaleV.size() != lanes) m_autoScaleV.fill(0.0, lanes);

    QPainter p(&m_traceLayer);
    if (m_fade) {
        // Multiply the whole layer's alpha down so older traces dim out
        // instead of accumulating into a solid block.
        ++m_fadeTick;
        if (m_fadeTick % 3 == 0) {
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.fillRect(rect(), QColor(0, 0, 0, 244));
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
    }

    QVector<float> flat;
    bool drewAny = false;
    const int firstLane = m_focusLane >= 0 ? m_focusLane : 0;
    const int endLane = m_focusLane >= 0 ? qMin(lanes, m_focusLane + 1) : lanes;
    for (int lane = firstLane; lane < endLane; ++lane) {
        quint64 seq = m_seen[lane];
        const int fresh =
            m_store->fetchNew(lane, &seq, &flat, m_incrementalDrawLimit);
        m_seen[lane] = seq;
        if (fresh > 0) {
            drewAny = true;
            drawSnippets(p, lane, flat, fresh);
        }
    }
    p.end();
    if (drewAny || m_fade) update();
}

void SpikeGridView::rebuildAll() {
    if (!m_store) {
        update();
        return;
    }
    m_layersValid = false;
    ensureLayers();
    if (m_traceLayer.isNull()) return;

    const int lanes = m_store->channels();
    m_seen.fill(0, lanes);
    if (m_autoScale) m_autoScaleV.fill(0.0, lanes);
    m_traceLayer.fill(Qt::transparent);

    QPainter p(&m_traceLayer);
    QVector<float> flat;
    if (m_focusLane == -1) {
        p.end();
        update();
        return;
    }
    const int firstLane = m_focusLane >= 0 ? m_focusLane : 0;
    const int endLane = m_focusLane >= 0 ? qMin(lanes, m_focusLane + 1) : lanes;
    for (int lane = firstLane; lane < endLane; ++lane) {
        quint64 seq = 0;
        const int have =
            m_store->fetchNew(lane, &seq, &flat, m_rebuildDrawLimit);
        m_seen[lane] = seq;
        if (have > 0) drawSnippets(p, lane, flat, have);
    }
    p.end();
    update();
}

void SpikeGridView::clearTraces() {
    if (m_traceLayer.isNull()) return;
    m_traceLayer.fill(Qt::transparent);
    const int lanes = m_store ? m_store->channels() : 0;
    m_seen.fill(0, lanes);
    m_autoScaleV.fill(0.0, lanes);
    update();
}

void SpikeGridView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    m_layersValid = false;
    rebuildAll();
}

void SpikeGridView::mousePressEvent(QMouseEvent *event) {
    if (m_thresholdEditingEnabled && m_focusLane >= 0 &&
        event->button() == Qt::LeftButton) {
        m_draggingThreshold = true;
        grabMouse();
        updateDraggedThreshold(event->pos());
        event->accept();
        return;
    }

    const int lanes = m_store ? m_store->channels() : 0;
    if (lanes <= 0 || m_cellW <= 0 || m_cellH <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->pos();
    const int col = (pos.x() - m_marginL) / m_cellW;
    const int row = (pos.y() - m_marginT) / m_cellH;
    if (col < 0 || col >= m_effColumns || row < 0 || row >= m_rows) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int lane = row * m_effColumns + col;
    if (lane >= 0 && lane < lanes) {
        setSelectedLane(lane);
        emit laneClicked(lane);
    }
    QWidget::mousePressEvent(event);
}

void SpikeGridView::mouseMoveEvent(QMouseEvent *event) {
    if (m_draggingThreshold && (event->buttons() & Qt::LeftButton)) {
        updateDraggedThreshold(event->pos());
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void SpikeGridView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_draggingThreshold && event->button() == Qt::LeftButton) {
        updateDraggedThreshold(event->pos());
        m_draggingThreshold = false;
        releaseMouse();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SpikeGridView::updateDraggedThreshold(const QPoint &position) {
    if (!m_thresholdEditingEnabled || m_focusLane < 0) return;
    const CellRect r = cellRect(m_focusLane);
    if (r.w <= 2 || r.h <= 2) return;

    const int plotY = r.y + 1;
    const int plotH = r.h - 2;
    const double half = qMax(1e-9, laneScale(m_focusLane) * 0.5);
    const double normalized =
        qBound(-1.0,
               (plotY + plotH * 0.5 - position.y()) /
                   qMax(1.0, plotH * 0.5),
               1.0);
    const double thresholdV = normalized * half;
    if (m_thresholds.size() <= m_focusLane) {
        m_thresholds.resize(m_focusLane + 1);
    }
    m_thresholds[m_focusLane] = thresholdV;
    update();
    emit thresholdDragged(thresholdV);
}

void SpikeGridView::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter p(this);
    ensureLayers();
    if (m_bgPixmap.isNull()) {
        p.fillRect(rect(), m_plotBg);
        return;
    }
    p.drawPixmap(0, 0, m_bgPixmap);
    if (!m_traceLayer.isNull()) {
        p.drawPixmap(0, 0, m_traceLayer);
    }

    const int lanes = m_store ? m_store->channels() : 0;
    if (lanes <= 0) {
        p.setPen(m_axis);
        p.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }
    if (m_focusLane == -1) {
        p.setPen(m_axis);
        p.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    // Threshold lines and rate readouts change every refresh, so they are
    // painted live rather than baked into the cached layers.
    const bool showLabels =
        m_focusLane >= 0 || (m_cellW >= kLabelMinW && m_cellH >= kLabelMinH);
    QFont f = p.font();
    f.setPixelSize(qBound(8, m_cellH / 5, 11));
    p.setFont(f);

    const int firstLane = m_focusLane >= 0 ? m_focusLane : 0;
    const int endLane = m_focusLane >= 0 ? qMin(lanes, m_focusLane + 1) : lanes;
    for (int lane = firstLane; lane < endLane; ++lane) {
        const CellRect r = cellRect(lane);
        if (r.w <= 2 || r.h <= 2) continue;
        const double thr = lane < m_thresholds.size() ? m_thresholds[lane] : 0.0;
        if (thr != 0.0) {
            const double half = qMax(1e-9, laneScale(lane) * 0.5);
            const double rawY =
                (r.y + 1) + (r.h - 2) * 0.5 -
                (thr / half) * ((r.h - 2) * 0.5);
            const bool clipped = rawY <= r.y + 2 || rawY >= r.y + r.h - 3;
            const double y =
                qBound<double>(r.y + 2, rawY, r.y + r.h - 3);
            QColor c = m_threshColor;
            c.setAlpha(190);
            p.setPen(QPen(c, clipped ? 2 : 1, Qt::DashLine));
            p.drawLine(QPointF(r.x + 1, y), QPointF(r.x + r.w - 2, y));
            if (clipped && m_focusLane >= 0) {
                const QString marker =
                    QStringLiteral("%1 %2 µV")
                        .arg(rawY < y ? QStringLiteral("↑")
                                      : QStringLiteral("↓"))
                        .arg(thr * 1e6, 0, 'f', 1);
                p.setPen(c);
                p.drawText(
                    QRect(r.x + 6,
                          rawY < y ? r.y + 4 : r.y + r.h - 20,
                          r.w - 12, 16),
                    rawY < y ? Qt::AlignTop | Qt::AlignLeft
                             : Qt::AlignBottom | Qt::AlignLeft,
                    marker);
            }
        }
        if (showLabels) {
            const double rate = lane < m_rates.size() ? m_rates[lane] : 0.0;
            if (rate > 0.05) {
                p.setPen(rate >= 1.0 ? m_wave : m_axis);
                const QString txt = rate >= 100.0 ? QString::number(rate, 'f', 0)
                                                  : QString::number(rate, 'f', 1);
                p.drawText(QRect(r.x, r.y + r.h - f.pixelSize() - 3, r.w - 3, f.pixelSize() + 2),
                           Qt::AlignRight | Qt::AlignVCenter, txt);
            }
        }
    }

    if (m_selected >= 0 && m_selected < lanes) {
        const CellRect r = cellRect(m_selected);
        p.setPen(QPen(m_selectColor, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.x + 1, r.y + 1, r.w - 3, r.h - 3);
    }
}

}  // namespace ccv2
