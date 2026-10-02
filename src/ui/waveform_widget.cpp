#include "ui/waveform_widget.h"

#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <cmath>
#include <limits>

namespace ccv2 {

namespace {

QString tickText(double value) {
    if (!std::isfinite(value)) {
        return QStringLiteral("0");
    }

    const double av = std::abs(value);
    if (av >= 1000.0) {
        return QString::number(value, 'f', 0);
    }
    if (av >= 100.0) {
        return QString::number(value, 'f', 1);
    }
    if (av >= 10.0) {
        return QString::number(value, 'f', 2);
    }
    return QString::number(value, 'g', 4);
}

QString frequencyTickText(double value)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return QStringLiteral("0");
    }
    if (value >= 1'000'000.0) {
        return QStringLiteral("%1M").arg(value / 1'000'000.0, 0, 'g', 3);
    }
    if (value >= 1000.0) {
        return QStringLiteral("%1k").arg(value / 1000.0, 0, 'g', 3);
    }
    return tickText(value);
}

// Y data is log10(value in SI base units); render the value itself with an
// engineering suffix, e.g. -7.5 -> "31.6n", -6 -> "1µ".
QString log10TickText(double exponent)
{
    if (!std::isfinite(exponent)) {
        return QStringLiteral("-");
    }
    const double value = std::pow(10.0, exponent);
    if (value >= 1.0) {
        return QString::number(value, 'g', 3);
    }
    if (value >= 1e-3) {
        return QStringLiteral("%1m").arg(value * 1e3, 0, 'g', 3);
    }
    if (value >= 1e-6) {
        return QStringLiteral("%1µ").arg(value * 1e6, 0, 'g', 3);
    }
    if (value >= 1e-9) {
        return QStringLiteral("%1n").arg(value * 1e9, 0, 'g', 3);
    }
    return QStringLiteral("%1p").arg(value * 1e12, 0, 'g', 3);
}

}  // namespace

WaveformWidget::WaveformWidget(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title) {
    m_palette = {
        {QStringLiteral("bg"), QStringLiteral("#10111d")},
        {QStringLiteral("plotBg"), QStringLiteral("#11131f")},
        {QStringLiteral("title"), QStringLiteral("#c5d2b8")},
        {QStringLiteral("grid"), QStringLiteral("#303342")},
        {QStringLiteral("minorGrid"), QStringLiteral("#1c1f2b")},
        {QStringLiteral("wave"), QStringLiteral("#63e6be")},
        {QStringLiteral("axis"), QStringLiteral("#c9cede")},
        {QStringLiteral("border"), QStringLiteral("#4b5064")},
    };
    setMinimumHeight(120);
    setMouseTracking(true);
    setToolTip(QStringLiteral("右键拖拽框选区域放大；右键单击或双击恢复"));
}

void WaveformWidget::setTitle(const QString &title) {
    m_title = title;
    update();
}

void WaveformWidget::setData(const QVector<double> &data) {
    m_data = data;
    m_series.clear();
    if (!m_data.isEmpty()) {
        PlotSeries s;
        s.data = m_data;
        s.color = QColor(m_palette.value(QStringLiteral("wave"), QStringLiteral("#00e5ff")));
        s.label = m_title;
        m_series.push_back(s);
    }
    update();
}

void WaveformWidget::setSeries(const QVector<PlotSeries> &series) {
    m_series.clear();
    m_series.reserve(series.size());
    for (const PlotSeries &seriesItem : series) {
        m_series.push_back(seriesItem);
    }
    m_data = (m_series.isEmpty() ? QVector<double>{} : m_series.first().data);
    update();
}

void WaveformWidget::setYRange(double yMin, double yMax) {
    if (yMax <= yMin) {
        return;
    }
    if (qFuzzyCompare(m_yMin, yMin) && qFuzzyCompare(m_yMax, yMax)) {
        return;
    }
    m_yMin = yMin;
    m_yMax = yMax;
    update();
}

void WaveformWidget::setXRange(double xMin, double xMax) {
    if (xMax <= xMin) {
        if (!m_hasXRange) {
            return;
        }
        m_hasXRange = false;
        update();
        return;
    }
    if (m_hasXRange && qFuzzyCompare(m_xMin, xMin) && qFuzzyCompare(m_xMax, xMax)) {
        return;
    }
    m_xMin = xMin;
    m_xMax = xMax;
    m_hasXRange = true;
    update();
}

void WaveformWidget::setAxisLabels(const QString &xLabel, const QString &yLabel) {
    if (m_xLabel == xLabel && m_yLabel == yLabel) {
        return;
    }
    m_xLabel = xLabel;
    m_yLabel = yLabel;
    update();
}

void WaveformWidget::setMaxRenderPoints(int points) {
    m_maxRenderPoints = points <= 0 ? 0 : qMax(64, points);
}

void WaveformWidget::setYLog10Labels(bool enabled) {
    if (m_yLog10Labels == enabled) {
        return;
    }
    m_yLog10Labels = enabled;
    update();
}

void WaveformWidget::setPaletteColors(const QMap<QString, QString> &palette) {
    for (auto it = palette.cbegin(); it != palette.cend(); ++it) {
        m_palette[it.key()] = it.value();
    }
    update();
}

void WaveformWidget::setDenseGrid(bool enabled) {
    if (m_denseGrid == enabled) {
        return;
    }
    m_denseGrid = enabled;
    update();
}

void WaveformWidget::setLogXScale(bool enabled) {
    if (m_logXScale == enabled) {
        return;
    }
    m_logXScale = enabled;
    update();
}

QRect WaveformWidget::plotRectForRange(double yMin, double yMax) const
{
    QFont plotFont(QStringLiteral("Microsoft YaHei UI"));
    plotFont.setPointSize(8);
    const QFontMetrics fm(plotFont);
    const int yMaxText = fm.horizontalAdvance(tickText(yMax));
    const int yMinText = fm.horizontalAdvance(tickText(yMin));
    const int leftMargin = qMax(62, qMax(yMaxText, yMinText) + 24);
    return rect().adjusted(leftMargin, 28, -14, -34);
}

void WaveformWidget::resetZoom()
{
    if (!m_zoomActive && !m_draggingZoom) {
        return;
    }
    m_zoomActive = false;
    m_draggingZoom = false;
    update();
}

QPair<double, double> WaveformWidget::displayedXRange() const
{
    int maxSamples = 0;
    for (const PlotSeries &series : m_series) {
        maxSamples = qMax(maxSamples, series.data.size());
    }
    double xMin = m_zoomActive ? m_zoomXMin : (m_hasXRange ? m_xMin : 0.0);
    double xMax = m_zoomActive ? m_zoomXMax
                               : (m_hasXRange ? m_xMax : static_cast<double>(qMax(1, maxSamples - 1)));
    if (m_logXScale) {
        double firstPositiveX = std::numeric_limits<double>::infinity();
        for (const PlotSeries &series : m_series) {
            const bool hasXData = series.xData.size() == series.data.size();
            for (int i = 0; i < series.data.size(); ++i) {
                const double candidate = hasXData ? series.xData[i] : static_cast<double>(i);
                if (candidate > 0.0 && candidate < firstPositiveX) {
                    firstPositiveX = candidate;
                }
            }
        }
        if (!std::isfinite(firstPositiveX)) {
            firstPositiveX = 1.0;
        }
        // Clamp to the log-representable domain but keep the zoom window:
        // overwriting xMin with the full-range minimum here is what used to
        // snap the left edge back on every horizontal zoom.
        xMin = qMax(xMin, firstPositiveX);
        xMax = qMax(xMax, xMin * 1.01);
    }
    return {xMin, xMax};
}

QVector<double> WaveformWidget::downsample(const QVector<double> &data) const {
    if (m_maxRenderPoints <= 0) {
        return data;
    }
    if (data.size() <= m_maxRenderPoints) {
        return data;
    }

    const int step = qMax(1, (data.size() + m_maxRenderPoints - 1) / m_maxRenderPoints);
    QVector<double> sampled;
    sampled.reserve((data.size() + step - 1) / step + 1);
    for (int i = 0; i < data.size(); i += step) {
        sampled.push_back(data[i]);
    }
    if (!sampled.isEmpty() && sampled.back() != data.back()) {
        sampled.push_back(data.back());
    }
    return sampled;
}

WaveformWidget::PlotSeries WaveformWidget::downsampleSeries(const PlotSeries &series) const {
    if (m_maxRenderPoints <= 0 || series.data.size() <= m_maxRenderPoints) {
        return series;
    }

    const int step = qMax(1, (series.data.size() + m_maxRenderPoints - 1) / m_maxRenderPoints);
    PlotSeries sampled;
    sampled.color = series.color;
    sampled.label = series.label;
    sampled.data.reserve((series.data.size() + step - 1) / step + 1);
    const bool hasX = series.xData.size() == series.data.size();
    sampled.xData.reserve(sampled.data.capacity());
    const double sourceMin = m_hasXRange ? m_xMin : 0.0;
    const double sourceMax = m_hasXRange ? m_xMax : qMax<qsizetype>(1, series.data.size() - 1);
    const auto sampleX = [&](qsizetype i) {
        return hasX ? series.xData[i]
                    : sourceMin + static_cast<double>(i) / (series.data.size() - 1) * (sourceMax - sourceMin);
    };

    int lastIndex = -1;
    for (int i = 0; i < series.data.size(); i += step) {
        sampled.data.push_back(series.data[i]);
        sampled.xData.push_back(sampleX(i));
        lastIndex = i;
    }
    if (lastIndex != series.data.size() - 1) {
        sampled.data.push_back(series.data.back());
        sampled.xData.push_back(sampleX(series.data.size() - 1));
    }
    return sampled;
}

void WaveformWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    QFont plotFont(QStringLiteral("Microsoft YaHei UI"));
    plotFont.setPointSize(8);
    painter.setFont(plotFont);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRect widgetRect = this->rect();

    painter.fillRect(widgetRect, QColor(m_palette.value(QStringLiteral("bg"), QStringLiteral("#0c1a2e"))));
    painter.setPen(QPen(QColor(m_palette.value(QStringLiteral("border"), QStringLiteral("#4b5064"))), 1));
    painter.drawRect(widgetRect.adjusted(0, 0, -1, -1));

    const QColor titleColor(m_palette.value(QStringLiteral("title"), QStringLiteral("#a0b8d0")));
    const QColor plotBgColor(m_palette.value(QStringLiteral("plotBg"), m_palette.value(QStringLiteral("bg"), QStringLiteral("#0c1a2e"))));
    const QColor gridColor(m_palette.value(QStringLiteral("grid"), QStringLiteral("#1c3350")));
    const QColor minorGridColor(m_palette.value(QStringLiteral("minorGrid"), QStringLiteral("#18273c")));
    const QColor axisColor(m_palette.value(QStringLiteral("axis"), QStringLiteral("#8faaca")));
    QFont titleFont(QStringLiteral("Microsoft YaHei UI"));
    titleFont.setPointSize(9);
    titleFont.setBold(false);
    painter.setFont(titleFont);
    painter.setPen(titleColor);
    painter.drawText(QRect(0, 6, width(), 18), Qt::AlignHCenter | Qt::AlignVCenter, m_title);
    painter.setFont(plotFont);

    const QFontMetrics fm(painter.font());
    const double displayYMin = m_zoomActive ? m_zoomYMin : m_yMin;
    const double displayYMax = m_zoomActive ? m_zoomYMax : m_yMax;
    const QRect plotRect = plotRectForRange(displayYMin, displayYMax);
    if (plotRect.width() < 20 || plotRect.height() < 20) {
        return;
    }

    painter.fillRect(plotRect, plotBgColor);

    const QPair<double, double> xRange = displayedXRange();
    const double xMin = xRange.first;
    const double xMax = xRange.second;
    const double xSpan = qMax(1e-12, xMax - xMin);
    const double logXMin = m_logXScale ? std::log10(qMax(1e-12, xMin)) : 0.0;
    const double logXMax = m_logXScale ? std::log10(qMax(xMin * 1.000001, xMax)) : 1.0;
    const double logXSpan = qMax(1e-12, logXMax - logXMin);
    const auto xToPixel = [&](double xValue) -> double {
        if (m_logXScale) {
            if (!std::isfinite(xValue) || xValue <= 0.0) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return plotRect.left() + ((std::log10(xValue) - logXMin) / logXSpan) * plotRect.width();
        }
        return plotRect.left() + ((xValue - xMin) / xSpan) * plotRect.width();
    };

    if (m_denseGrid) {
        painter.setPen(QPen(minorGridColor, 1));
        for (int i = 1; i < 20; ++i) {
            if (i % 4 == 0) {
                continue;
            }
            const int y = plotRect.top() + plotRect.height() * i / 20;
            painter.drawLine(plotRect.left(), y, plotRect.right(), y);
        }
        if (m_logXScale) {
            const int startExp = static_cast<int>(std::floor(logXMin));
            const int endExp = static_cast<int>(std::ceil(logXMax));
            for (int exp = startExp; exp <= endExp; ++exp) {
                const double base = std::pow(10.0, exp);
                for (int mul = 2; mul < 10; ++mul) {
                    const double value = base * mul;
                    if (value <= xMin || value >= xMax) {
                        continue;
                    }
                    const double px = xToPixel(value);
                    if (std::isfinite(px)) {
                        painter.drawLine(QPointF(px, plotRect.top()), QPointF(px, plotRect.bottom()));
                    }
                }
            }
        } else {
            for (int i = 1; i < 40; ++i) {
                if (i % 4 == 0) {
                    continue;
                }
                const int x = plotRect.left() + plotRect.width() * i / 40;
                painter.drawLine(x, plotRect.top(), x, plotRect.bottom());
            }
        }
    }

    painter.setPen(QPen(gridColor, 1));
    for (int i = 1; i < 5; ++i) {
        const int y = plotRect.top() + plotRect.height() * i / 5;
        painter.drawLine(plotRect.left(), y, plotRect.right(), y);
    }
    if (m_logXScale) {
        const int startExp = static_cast<int>(std::ceil(logXMin));
        const int endExp = static_cast<int>(std::floor(logXMax));
        for (int exp = startExp; exp <= endExp; ++exp) {
            const double value = std::pow(10.0, exp);
            if (value <= xMin || value >= xMax) {
                continue;
            }
            const double px = xToPixel(value);
            if (std::isfinite(px)) {
                painter.drawLine(QPointF(px, plotRect.top()), QPointF(px, plotRect.bottom()));
            }
        }
    } else {
        for (int i = 1; i < 10; ++i) {
            const int x = plotRect.left() + plotRect.width() * i / 10;
            painter.drawLine(x, plotRect.top(), x, plotRect.bottom());
        }
    }

    painter.setPen(QPen(axisColor, 1));
    painter.drawRect(plotRect.adjusted(0, 0, -1, -1));

    const double ySpan = qMax(1e-12, displayYMax - displayYMin);
    painter.setPen(axisColor);
    for (int i = 0; i <= 5; ++i) {
        const int y = plotRect.top() + plotRect.height() * i / 5;
        const double value = displayYMax - ySpan * static_cast<double>(i) / 5.0;
        painter.drawLine(plotRect.left() - 4, y, plotRect.left(), y);
        painter.drawText(QRect(2, y - fm.height() / 2, plotRect.left() - 10, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         m_yLog10Labels ? log10TickText(value) : tickText(value));
    }

    if (m_logXScale) {
        const int startExp = static_cast<int>(std::floor(logXMin));
        const int endExp = static_cast<int>(std::ceil(logXMax));
        for (int exp = startExp; exp <= endExp; ++exp) {
            const double base = std::pow(10.0, exp);
            for (double mul : {1.0, 2.0, 5.0}) {
                const double value = base * mul;
                if (value < xMin || value > xMax) {
                    continue;
                }
                const double px = xToPixel(value);
                if (!std::isfinite(px) || px < plotRect.left() - 1 || px > plotRect.right() + 1) {
                    continue;
                }
                painter.drawLine(QPointF(px, plotRect.bottom()), QPointF(px, plotRect.bottom() + 4));
                painter.drawText(QRectF(px - 42, plotRect.bottom() + 6, 84, fm.height()),
                                 Qt::AlignHCenter | Qt::AlignTop,
                                 frequencyTickText(value));
            }
        }
    } else {
        for (int i = 0; i <= 10; ++i) {
            if (i % 2 != 0 && width() < 1100) {
                continue;
            }
            const int x = plotRect.left() + plotRect.width() * i / 10;
            const double value = xMin + xSpan * static_cast<double>(i) / 10.0;
            painter.drawLine(x, plotRect.bottom(), x, plotRect.bottom() + 4);
            painter.drawText(QRect(x - 42, plotRect.bottom() + 6, 84, fm.height()),
                             Qt::AlignHCenter | Qt::AlignTop,
                             tickText(value));
        }
    }

    if (!m_xLabel.isEmpty()) {
        painter.drawText(QRect(plotRect.left(), plotRect.bottom() + 18, plotRect.width(), fm.height()),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         m_xLabel);
    }
    if (!m_yLabel.isEmpty()) {
        painter.drawText(QRect(plotRect.left() + 6, plotRect.top() + 4, 180, fm.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         m_yLabel);
    }

    if (m_series.isEmpty()) {
        return;
    }

    // Draw at true coordinates and let the painter clip: the zoomed window
    // must be filled edge-to-edge, so keep one anchor point beyond each side
    // instead of dropping out-of-range points (x is monotonic for all series
    // we plot), and never clamp Y — clipping trims overshoot naturally.
    painter.save();
    painter.setClipRect(plotRect);
    for (const PlotSeries &sourceSeries : m_series) {
        if (sourceSeries.data.size() < 2) {
            continue;
        }

        const QColor color = sourceSeries.color.isValid() ? sourceSeries.color : QColor(m_palette.value(QStringLiteral("wave"), QStringLiteral("#00e5ff")));
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(color, 1.2));

        const PlotSeries series = downsampleSeries(sourceSeries);

        QPolygonF poly;
        poly.reserve(series.data.size());
        const auto flushPolyline = [&]() {
            if (poly.size() >= 2) {
                painter.drawPolyline(poly);
            }
            poly.clear();
        };
        const int n = series.data.size();
        const bool hasXData = series.xData.size() == series.data.size();
        const double sourceXMin = m_hasXRange ? m_xMin : 0.0;
        const double sourceXMax = m_hasXRange ? m_xMax : qMax(1, n - 1);
        bool havePending = false;
        QPointF pendingLeft;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(series.data[i])) {
                flushPolyline();
                havePending = false;
                continue;
            }
            const double xValue = hasXData
                                      ? series.xData[i]
                                      : (sourceXMin + (static_cast<double>(i) / static_cast<double>(n - 1)) * (sourceXMax - sourceXMin));
            const double x = xToPixel(xValue);
            if (!std::isfinite(x)) {
                flushPolyline();
                havePending = false;
                continue;
            }
            const qreal y = plotRect.bottom()
                            - ((static_cast<double>(series.data[i]) - displayYMin) / ySpan) * plotRect.height();
            if (x < plotRect.left()) {
                pendingLeft = QPointF(x, y);  // nearest point left of the window
                havePending = true;
                continue;
            }
            if (poly.isEmpty() && havePending) {
                poly << pendingLeft;
                havePending = false;
            }
            poly << QPointF(x, y);
            if (x > plotRect.right()) {
                break;  // right anchor emitted; the rest is offscreen
            }
        }
        flushPolyline();
        painter.setRenderHint(QPainter::Antialiasing, false);
    }
    painter.restore();

    if (m_series.size() > 1 && m_series.size() <= 32) {
        const int rowH = fm.height() + 4;
        const int legendW = qMin(plotRect.width() - 12, 260);
        const int legendH = qMin(plotRect.height() - 12, rowH * m_series.size() + 8);
        QRect legendRect(plotRect.right() - legendW - 8, plotRect.top() + 8, legendW, legendH);
        QColor legendBg(m_palette.value(QStringLiteral("bg"), QStringLiteral("#10111d")));
        legendBg.setAlpha(210);
        painter.fillRect(legendRect, legendBg);
        painter.setPen(QPen(axisColor, 1));
        painter.drawRect(legendRect.adjusted(0, 0, -1, -1));

        int y = legendRect.top() + 6;
        for (const PlotSeries &series : m_series) {
            if (y + rowH > legendRect.bottom()) {
                break;
            }
            const QColor color = series.color.isValid() ? series.color : QColor(m_palette.value(QStringLiteral("wave"), QStringLiteral("#00e5ff")));
            painter.setPen(QPen(color, 2));
            painter.drawLine(legendRect.left() + 8, y + rowH / 2, legendRect.left() + 28, y + rowH / 2);
            painter.setPen(axisColor);
            const QString label = series.label.isEmpty() ? QStringLiteral("Series") : series.label;
            painter.drawText(QRect(legendRect.left() + 34, y, legendRect.width() - 40, rowH),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             label);
            y += rowH;
        }
    }

    if (m_draggingZoom) {
        QRect selection = QRect(m_zoomDragStart, m_zoomDragCurrent).normalized().intersected(plotRect);
        if (selection.width() > 3 && selection.height() > 3) {
            QColor fill(m_palette.value(QStringLiteral("primary"), QStringLiteral("#6ea8ff")));
            if (!fill.isValid()) {
                fill = QColor(0x6e, 0xa8, 0xff);
            }
            fill.setAlpha(42);
            QColor edge = fill;
            edge.setAlpha(210);
            painter.fillRect(selection, fill);
            painter.setPen(QPen(edge, 1));
            painter.drawRect(selection.adjusted(0, 0, -1, -1));
        }
    }
}

void WaveformWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::RightButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const double displayYMin = m_zoomActive ? m_zoomYMin : m_yMin;
    const double displayYMax = m_zoomActive ? m_zoomYMax : m_yMax;
    const QRect plotRect = plotRectForRange(displayYMin, displayYMax);
    if (!plotRect.contains(event->pos())) {
        resetZoom();
        event->accept();
        return;
    }

    m_draggingZoom = true;
    m_zoomDragStart = event->pos();
    m_zoomDragCurrent = event->pos();
    update();
    event->accept();
}

void WaveformWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_draggingZoom) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    m_zoomDragCurrent = event->pos();
    update();
    event->accept();
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::RightButton || !m_draggingZoom) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    const double currentYMin = m_zoomActive ? m_zoomYMin : m_yMin;
    const double currentYMax = m_zoomActive ? m_zoomYMax : m_yMax;
    const QRect plotRect = plotRectForRange(currentYMin, currentYMax);
    const QRect selection = QRect(m_zoomDragStart, event->pos()).normalized().intersected(plotRect);
    m_draggingZoom = false;

    if (selection.width() < 8 || selection.height() < 8) {
        resetZoom();
        event->accept();
        return;
    }

    // Must match the range paintEvent rendered, or the selected box maps to
    // the wrong data window.
    const QPair<double, double> xRange = displayedXRange();
    const double baseXMin = xRange.first;
    const double baseXMax = xRange.second;

    const double leftRatio = qBound(0.0,
                                    static_cast<double>(selection.left() - plotRect.left()) / qMax(1, plotRect.width()),
                                    1.0);
    const double rightRatio = qBound(0.0,
                                     static_cast<double>(selection.right() - plotRect.left()) / qMax(1, plotRect.width()),
                                     1.0);
    const double topRatio = qBound(0.0,
                                   static_cast<double>(selection.top() - plotRect.top()) / qMax(1, plotRect.height()),
                                   1.0);
    const double bottomRatio = qBound(0.0,
                                      static_cast<double>(selection.bottom() - plotRect.top()) / qMax(1, plotRect.height()),
                                      1.0);

    double newXMin = baseXMin;
    double newXMax = baseXMax;
    if (m_logXScale) {
        const double logMin = std::log10(qMax(1e-12, baseXMin));
        const double logMax = std::log10(qMax(baseXMin * 1.000001, baseXMax));
        const double logSpan = qMax(1e-12, logMax - logMin);
        newXMin = std::pow(10.0, logMin + logSpan * leftRatio);
        newXMax = std::pow(10.0, logMin + logSpan * rightRatio);
    } else {
        const double xSpan = qMax(1e-12, baseXMax - baseXMin);
        newXMin = baseXMin + xSpan * leftRatio;
        newXMax = baseXMin + xSpan * rightRatio;
    }

    const double ySpan = qMax(1e-12, currentYMax - currentYMin);
    const double newYMax = currentYMax - ySpan * topRatio;
    const double newYMin = currentYMax - ySpan * bottomRatio;

    if (newXMax > newXMin && newYMax > newYMin) {
        m_zoomXMin = newXMin;
        m_zoomXMax = newXMax;
        m_zoomYMin = newYMin;
        m_zoomYMax = newYMax;
        m_zoomActive = true;
    }

    update();
    event->accept();
}

void WaveformWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        resetZoom();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

}  // namespace ccv2
