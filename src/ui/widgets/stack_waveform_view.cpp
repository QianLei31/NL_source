#include "stack_waveform_view.h"

#include <QFontMetrics>
#include <QPainter>

#include <cmath>

namespace ccv2 {

namespace {

QColor fallbackTraceColor(int index, int count)
{
    const qreal h = std::fmod(200.0 + index * 360.0 / std::max(1, count) * 0.62, 360.0);
    return QColor::fromHslF(h / 360.0, 0.62, 0.58);
}

QVector<double> downsampleData(const QVector<double> &data)
{
    constexpr int maxRenderPoints = 1800;
    if (data.size() <= maxRenderPoints) {
        return data;
    }

    const int step = qMax(1, (data.size() + maxRenderPoints - 1) / maxRenderPoints);
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

}  // namespace

StackWaveformView::StackWaveformView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(260, 320);
    setProperty("class", "waveform-plot");
    m_theme = {
        {QStringLiteral("bg"), QStringLiteral("#10111d")},
        {QStringLiteral("plotBg"), QStringLiteral("#11131f")},
        {QStringLiteral("grid"), QStringLiteral("#253246")},
        {QStringLiteral("axis"), QStringLiteral("#8faaca")},
        {QStringLiteral("wave"), QStringLiteral("#63e6be")},
        {QStringLiteral("title"), QStringLiteral("#a8c0dd")},
        {QStringLiteral("border"), QStringLiteral("#30363d")},
    };
}

void StackWaveformView::setChannelCount(int n)
{
    m_channelCount = n;
    m_data.resize(n);
    m_traces.clear();
    if (m_colors.size() < n) {
        m_colors.resize(n);
        for (int i = 0; i < n; ++i) {
            m_colors[i] = fallbackTraceColor(i, n);
        }
    }
    update();
}

void StackWaveformView::setPalette(const QVector<QColor> &colors)
{
    m_colors = colors;
    update();
}

void StackWaveformView::setThemePalette(const QMap<QString, QString> &palette)
{
    for (auto it = palette.cbegin(); it != palette.cend(); ++it) {
        m_theme[it.key()] = it.value();
    }
    update();
}

void StackWaveformView::setPlaceholderText(const QString &text)
{
    m_placeholder = text;
    update();
}

void StackWaveformView::setTimeSpanSeconds(double seconds)
{
    m_timeSpanSeconds = qMax(0.0, seconds);
    update();
}

void StackWaveformView::setTraces(const QVector<Trace> &traces)
{
    m_traces = traces;
    m_channelCount = traces.size();
    m_data.clear();
    update();
}

void StackWaveformView::setYScaleMode(YScaleMode mode)
{
    if (m_yScaleMode == mode) {
        return;
    }
    m_yScaleMode = mode;
    update();
}

void StackWaveformView::setFixedYRange(double minimum, double maximum)
{
    if (!(maximum > minimum)) {
        return;
    }
    if (qFuzzyCompare(m_fixedYMin, minimum) && qFuzzyCompare(m_fixedYMax, maximum)) {
        return;
    }
    m_fixedYMin = minimum;
    m_fixedYMax = maximum;
    update();
}

void StackWaveformView::onPreviewBatch(const QVector<int> &channels, const QVector<QVector<double>> &samples)
{
    m_data.clear();
    m_data.reserve(samples.size());
    for (const QVector<double> &sample : samples) {
        m_data.push_back(downsampleData(sample));
    }
    m_channelCount = samples.size();
    m_traces.clear();
    if (m_colors.size() < m_channelCount) {
        m_colors.resize(m_channelCount);
        for (int i = 0; i < m_channelCount; ++i) {
            m_colors[i] = fallbackTraceColor(i, m_channelCount);
        }
    }
    for (int i = 0; i < m_channelCount && i < channels.size(); ++i) {
        if (!m_colors[i].isValid()) {
            m_colors[i] = fallbackTraceColor(i, m_channelCount);
        }
    }
    update();
}

QColor StackWaveformView::colorFromTheme(const QString &key, const QColor &fallback) const
{
    const QColor color(m_theme.value(key));
    return color.isValid() ? color : fallback;
}

void StackWaveformView::paintEvent(QPaintEvent *)
{
    QVector<Trace> traces = m_traces;
    if (traces.isEmpty() && !m_data.isEmpty()) {
        traces.reserve(m_data.size());
        for (int i = 0; i < m_data.size(); ++i) {
            Trace trace;
            trace.data = m_data[i];
            trace.label = QStringLiteral("CH%1").arg(i);
            trace.color = (i < m_colors.size() && m_colors[i].isValid())
                              ? m_colors[i]
                              : fallbackTraceColor(i, m_data.size());
            traces.push_back(trace);
        }
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QColor bg = colorFromTheme(QStringLiteral("plotBg"), QColor(0x0d, 0x13, 0x1b));
    const QColor panel = colorFromTheme(QStringLiteral("bg"), QColor(0x10, 0x11, 0x1d));
    const QColor grid = colorFromTheme(QStringLiteral("grid"), QColor(0x25, 0x32, 0x46));
    const QColor axis = colorFromTheme(QStringLiteral("axis"), QColor(0x8f, 0xaa, 0xca));
    const QColor text = colorFromTheme(QStringLiteral("title"), QColor(0xa8, 0xc0, 0xdd));
    const QColor border = colorFromTheme(QStringLiteral("border"), QColor(0x30, 0x36, 0x3d));

    p.fillRect(rect(), panel);
    p.setPen(QPen(border, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    if (traces.isEmpty()) {
        p.setPen(text);
        p.drawText(rect(), Qt::AlignCenter,
                   m_placeholder.isEmpty()
                       ? QStringLiteral("No data stream.\nClick Start to begin acquisition.")
                       : m_placeholder);
        p.end();
        return;
    }

    QFont labelFont = font();
    labelFont.setPixelSize(10);
    p.setFont(labelFont);
    const QFontMetrics fm(labelFont);

    const int count = traces.size();
    const int labelW = qMax(84, fm.horizontalAdvance(QStringLiteral("CH000 L0 ELE0000")) + 10);
    const int topPad = 6;
    const int bottomPad = 26;
    const int plotLeft = labelW;
    const int plotRight = width() - 8;
    const int plotW = qMax(20, plotRight - plotLeft);
    const double stripH = static_cast<double>(qMax(1, height() - topPad - bottomPad)) / count;

    for (int row = 0; row < count; ++row) {
        const Trace &trace = traces[row];
        const double stripTop = topPad + stripH * row;
        const double stripBottom = topPad + stripH * (row + 1);
        const double yCenter = (stripTop + stripBottom) * 0.5;
        const double yScale = qMax(2.0, stripH * 0.36);

        const QRectF rowRect(0.0, stripTop, width(), stripH);
        p.fillRect(rowRect.adjusted(0, 0, 0, -1), bg);

        QColor rowGrid = grid;
        rowGrid.setAlpha(row % 2 == 0 ? 90 : 55);
        p.setPen(QPen(rowGrid, 1));
        p.drawLine(plotLeft, static_cast<int>(yCenter), plotRight, static_cast<int>(yCenter));
        p.drawLine(plotLeft, static_cast<int>(stripBottom), plotRight, static_cast<int>(stripBottom));

        p.setPen(text);
        const QString label = trace.label.isEmpty() ? QStringLiteral("CH%1").arg(row) : trace.label;
        p.drawText(QRectF(8, stripTop, labelW - 14, stripH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   label);

        if (trace.data.size() < 2) {
            continue;
        }

        bool hasFinite = false;
        double minV = m_fixedYMin;
        double maxV = m_fixedYMax;
        if (m_yScaleMode == AdaptiveYScale) {
            for (double v : trace.data) {
                if (!std::isfinite(v)) {
                    continue;
                }
                if (!hasFinite) {
                    minV = v;
                    maxV = v;
                    hasFinite = true;
                } else {
                    minV = qMin(minV, v);
                    maxV = qMax(maxV, v);
                }
            }
        } else {
            hasFinite = true;
        }
        if (!hasFinite) {
            continue;
        }
        double range = maxV - minV;
        if (range < 1e-9) {
            range = 1.0;
            minV -= 0.5;
            maxV += 0.5;
        }

        const QColor color = trace.color.isValid()
                                 ? trace.color
                                 : colorFromTheme(QStringLiteral("wave"), fallbackTraceColor(row, count));
        p.setPen(QPen(color, 1.15));

        Trace rendered = trace;
        rendered.data = downsampleData(trace.data);
        const int renderedN = rendered.data.size();
        QVector<QPointF> points;
        points.reserve(renderedN);
        const auto flushPolyline = [&]() {
            if (points.size() >= 2) {
                p.drawPolyline(points.constData(), points.size());
            }
            points.clear();
        };
        for (int i = 0; i < renderedN; ++i) {
            if (!std::isfinite(rendered.data[i])) {
                flushPolyline();
                continue;
            }
            const double normalized = qBound(-1.0, ((rendered.data[i] - minV) / range) * 2.0 - 1.0, 1.0);
            const double x = plotLeft + static_cast<double>(i) / qMax(1, renderedN - 1) * plotW;
            const double y = yCenter - normalized * yScale;
            points.push_back(QPointF(x, y));
        }

        p.setRenderHint(QPainter::Antialiasing, true);
        flushPolyline();
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    p.setPen(QPen(axis, 1));
    p.drawLine(plotLeft, topPad, plotLeft, height() - bottomPad);
    const int axisY = height() - 14;
    p.drawLine(plotLeft, axisY, plotRight, axisY);
    p.drawLine(plotLeft, axisY - 4, plotLeft, axisY + 4);
    p.drawLine(plotRight, axisY - 4, plotRight, axisY + 4);

    const double spanMs = m_timeSpanSeconds * 1000.0;
    p.setPen(text);
    p.drawText(QRectF(plotLeft, axisY + 4, 80, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("0 ms"));
    p.drawText(QRectF(plotRight - 110, axisY + 4, 110, 14),
               Qt::AlignRight | Qt::AlignVCenter,
               spanMs >= 1000.0
                   ? QStringLiteral("%1 s").arg(m_timeSpanSeconds, 0, 'f', 2)
                   : QStringLiteral("%1 ms").arg(spanMs, 0, 'f', 1));
    p.drawText(QRectF(plotLeft, 2, plotW, 14),
               Qt::AlignRight | Qt::AlignVCenter,
               m_yScaleMode == AdaptiveYScale
                   ? QStringLiteral("Y: auto / trace")
                   : QStringLiteral("Y: %1 .. %2 V")
                         .arg(m_fixedYMin, 0, 'g', 3)
                         .arg(m_fixedYMax, 0, 'g', 3));
    p.end();
}

} // namespace ccv2
