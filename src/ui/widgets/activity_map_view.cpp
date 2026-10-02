#include "activity_map_view.h"

#include <QMouseEvent>
#include <QLinearGradient>
#include <QPainter>

#include "core/channel_routing.h"

#include <cmath>

#include "core/channel_routing.h"
#include "src/core/channel_info.h"

namespace ccv2 {

void ActivityMapView::clearMetrics()
{
    m_states.fill(ChannelState{}, 256);
    m_tdmStates.fill(ChannelState{}, kGlobalChannels * kTdmPhaseCount);
    m_maxRms = 1.0;
    m_maxP2p = 1.0;
    m_tdmMaxRms = 1.0;
    m_tdmMaxP2p = 1.0;
    update();
}

namespace {

constexpr int kPhysicalCols = 16;
constexpr int kBlocksPerPhysicalCol = 4;
constexpr int kLocalsPerBlock = 4;
constexpr int kPhysicalRows = kBlocksPerPhysicalCol * kLocalsPerBlock;

int blockForPhysicalCell(int col, int blockInCol)
{
    const int base = col * kBlocksPerPhysicalCol;
    return (col % 2 == 0)
               ? base + blockInCol
               : base + (kBlocksPerPhysicalCol - 1 - blockInCol);
}

int channelForPhysicalCell(int col, int row)
{
    const int blockInCol = row / kLocalsPerBlock;
    const int localChannel = row % kLocalsPerBlock;
    const int block = blockForPhysicalCell(col, blockInCol);
    const int channel = block * kLocalsPerBlock + localChannel;
    return (channel >= 0 && channel < kGlobalChannels) ? channel : -1;
}

} // namespace

ActivityMapView::ActivityMapView(QWidget *parent)
    : QWidget(parent)
    , m_states(kGlobalChannels)
    , m_tdmStates(kGlobalChannels * kTdmPhaseCount)
{
    setMinimumSize(250, 280);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void ActivityMapView::setPalette(const ThemePalette &palette)
{
    m_palette = palette;
    update();
}

void ActivityMapView::setPaletteColors(const QMap<QString, QString> &palette)
{
    for (auto it = palette.cbegin(); it != palette.cend(); ++it) {
        m_paletteColors[it.key()] = it.value();
    }
    update();
}

void ActivityMapView::setSelectedChannels(const QVector<int> &channels)
{
    m_selectedChannels.clear();
    for (int ch : channels) {
        if (ch >= 0 && ch < kGlobalChannels) {
            m_selectedChannels.insert(ch);
        }
    }
    update();
}

void ActivityMapView::setMetricMode(int mode)
{
    m_metricMode = qBound(static_cast<int>(MetricRms), mode, static_cast<int>(MetricSaturation));
    update();
}

void ActivityMapView::setTdmDisplay(bool enabled, bool pair02)
{
    if (m_tdmDisplay == enabled && m_pair02 == pair02) {
        return;
    }
    m_tdmDisplay = enabled;
    m_pair02 = pair02;
    update();
}

double ActivityMapView::maxRms() const
{
    return m_maxRms;
}

void ActivityMapView::onChannelMetrics(int globalChannel, double rms, double p2p, bool saturated, bool packetLoss)
{
    if (globalChannel < 0 || globalChannel >= kGlobalChannels) {
        return;
    }
    m_states[globalChannel].rms = rms;
    m_states[globalChannel].p2p = p2p;
    m_states[globalChannel].saturated = saturated;
    m_states[globalChannel].packetLoss = packetLoss;
    m_states[globalChannel].valid = true;
    if (rms > m_maxRms) {
        m_maxRms = rms;
    }
    update();
}

void ActivityMapView::setMetrics(const QVector<RealtimeChannelMetric> &metrics)
{
    const int n = qMin(metrics.size(), m_states.size());
    double currentMax = 1e-9;
    double currentMaxP2p = 1e-9;
    for (int ch = 0; ch < n; ++ch) {
        m_states[ch].mean = metrics[ch].mean;
        m_states[ch].rms = metrics[ch].rms;
        m_states[ch].p2p = metrics[ch].p2p;
        m_states[ch].saturated = metrics[ch].saturated;
        m_states[ch].packetLoss = metrics[ch].packetLoss;
        m_states[ch].valid = metrics[ch].valid;
        if (metrics[ch].valid) {
            currentMax = qMax(currentMax, metrics[ch].rms);
            currentMaxP2p = qMax(currentMaxP2p, metrics[ch].p2p);
        }
    }
    m_maxRms = currentMax;
    m_maxP2p = currentMaxP2p;
    update();
}

void ActivityMapView::setTdmMetrics(const QVector<RealtimeChannelMetric> &metrics)
{
    const int n = qMin(metrics.size(), m_tdmStates.size());
    double currentMax = 1e-9;
    double currentMaxP2p = 1e-9;
    for (int idx = 0; idx < n; ++idx) {
        m_tdmStates[idx].mean = metrics[idx].mean;
        m_tdmStates[idx].rms = metrics[idx].rms;
        m_tdmStates[idx].p2p = metrics[idx].p2p;
        m_tdmStates[idx].saturated = metrics[idx].saturated;
        m_tdmStates[idx].packetLoss = metrics[idx].packetLoss;
        m_tdmStates[idx].valid = metrics[idx].valid;
        if (metrics[idx].valid) {
            currentMax = qMax(currentMax, metrics[idx].rms);
            currentMaxP2p = qMax(currentMaxP2p, metrics[idx].p2p);
        }
    }
    m_tdmMaxRms = currentMax;
    m_tdmMaxP2p = currentMaxP2p;
    update();
}

QColor ActivityMapView::colorFromPalette(const QString &key, const QColor &fallback) const
{
    const QColor fromMap(m_paletteColors.value(key));
    if (fromMap.isValid()) {
        return fromMap;
    }
    if (key == QStringLiteral("plotBg") && m_palette.plotBg.isValid()) {
        return m_palette.plotBg;
    }
    if (key == QStringLiteral("border") && m_palette.cardBorder.isValid()) {
        return m_palette.cardBorder;
    }
    if (key == QStringLiteral("axis") && m_palette.plotAxis.isValid()) {
        return m_palette.plotAxis;
    }
    if (key == QStringLiteral("primary") && m_palette.primary.isValid()) {
        return m_palette.primary;
    }
    if (key == QStringLiteral("warning") && m_palette.warning.isValid()) {
        return m_palette.warning;
    }
    if (key == QStringLiteral("error") && m_palette.error.isValid()) {
        return m_palette.error;
    }
    return fallback;
}

QColor ActivityMapView::activityColor(double intensity) const
{
    intensity = qBound(0.0, intensity, 1.0);
    if (intensity < 0.5) {
        const double t = intensity / 0.5;
        return QColor::fromRgbF(0.03, 0.10 + 0.28 * t, 0.24 + 0.48 * t);
    }
    const double t = (intensity - 0.5) / 0.5;
    return QColor::fromRgbF(0.03 + 0.90 * t, 0.38 + 0.42 * (1.0 - std::abs(t - 0.35)), 0.72 * (1.0 - t));
}

double ActivityMapView::metricValue(const ChannelState &state) const
{
    if (m_metricMode == MetricP2p) {
        return state.p2p;
    }
    if (m_metricMode == MetricMean) {
        return state.mean;
    }
    if (m_metricMode == MetricSaturation) {
        return state.saturated ? 1.0 : 0.0;
    }
    return state.rms;
}

double ActivityMapView::metricMax() const
{
    if (m_metricMode == MetricP2p) {
        return m_tdmDisplay ? m_tdmMaxP2p : m_maxP2p;
    }
    if (m_metricMode == MetricMean) {
        return 1.8;
    }
    if (m_metricMode == MetricSaturation) {
        return 1.0;
    }
    return m_tdmDisplay ? m_tdmMaxRms : m_maxRms;
}

QString ActivityMapView::metricName() const
{
    if (m_metricMode == MetricP2p) {
        return QStringLiteral("p2p");
    }
    if (m_metricMode == MetricMean) {
        return QStringLiteral("mean");
    }
    if (m_metricMode == MetricSaturation) {
        return QStringLiteral("saturation");
    }
    return QStringLiteral("AC RMS");
}

QString ActivityMapView::metricScaleText() const
{
    if (m_metricMode == MetricSaturation) {
        return QStringLiteral("yellow=saturated  red=packet loss");
    }
    if (m_metricMode == MetricMean) {
        return QStringLiteral("0 V .. 1.8 V");
    }
    return QStringLiteral("0 .. %1").arg(voltageText(metricMax()));
}

QColor ActivityMapView::colorForState(const ChannelState &state,
                                      const QColor &warning,
                                      const QColor &error) const
{
    if (!state.valid) {
        return colorFromPalette(QStringLiteral("plotBg"), QColor(0x08, 0x15, 0x24)).lighter(118);
    }
    if (state.packetLoss) {
        return error;
    }
    if (state.saturated) {
        return warning;
    }

    double normalized = 0.0;
    if (m_metricMode == MetricMean) {
        normalized = qBound(0.0, state.mean / 1.8, 1.0);
    } else if (m_metricMode == MetricSaturation) {
        normalized = state.saturated ? 1.0 : 0.0;
    } else {
        const double maxValue = metricMax();
        normalized = maxValue > 1e-12 ? metricValue(state) / maxValue : 0.0;
    }
    const double intensity = (m_metricMode == MetricMean || m_metricMode == MetricSaturation)
                                 ? normalized
                                 : std::log1p(8.0 * normalized) / std::log(9.0);
    return activityColor(intensity);
}

QRectF ActivityMapView::mapRect() const
{
    const double topReserve = 24.0;
    const double bottomReserve = 48.0;
    const int side = qMax(1, qMin(width() - 8, static_cast<int>(height() - topReserve - bottomReserve)));
    return QRectF((width() - side) * 0.5 + 4.0,
                  topReserve,
                  side - 8.0,
                  side - 8.0);
}

int ActivityMapView::channelAt(const QPointF &point) const
{
    const QRectF mr = mapRect();
    if (!mr.contains(point)) {
        return -1;
    }
    const int col = qBound(0,
                           static_cast<int>((point.x() - mr.left()) / (mr.width() / kPhysicalCols)),
                           kPhysicalCols - 1);
    const int row = qBound(0,
                           static_cast<int>((point.y() - mr.top()) / (mr.height() / kPhysicalRows)),
                           kPhysicalRows - 1);
    return channelForPhysicalCell(col, row);
}

void ActivityMapView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF mr = mapRect();
    const double cellW = mr.width() / kPhysicalCols;
    const double cellH = mr.height() / kPhysicalRows;

    const QColor bgColor = colorFromPalette(QStringLiteral("plotBg"), QColor(0x08, 0x15, 0x24));
    const QColor borderColor = colorFromPalette(QStringLiteral("border"), QColor(0x26, 0x32, 0x3f));
    const QColor axisColor = colorFromPalette(QStringLiteral("axis"), QColor(0x86, 0x9a, 0xb3));
    const QColor primary = colorFromPalette(QStringLiteral("primary"), QColor(0x7a, 0xa7, 0xff));
    const QColor warning = colorFromPalette(QStringLiteral("warning"), QColor(0xe8, 0xb8, 0x6d));
    const QColor error = colorFromPalette(QStringLiteral("error"), QColor(0xff, 0x7b, 0x72));

    p.fillRect(rect(), bgColor.darker(112));
    p.setPen(QPen(borderColor, 1));
    p.drawRoundedRect(mr.adjusted(-1, -1, 1, 1), 6, 6);

    for (int col = 0; col < kPhysicalCols; ++col) {
        for (int row = 0; row < kPhysicalRows; ++row) {
            const int gch = channelForPhysicalCell(col, row);
            if (gch < 0) {
                continue;
            }

            QRectF r(mr.left() + col * cellW + 1.0,
                     mr.top() + row * cellH + 1.0,
                     cellW - 2.0,
                     cellH - 2.0);

            if (m_tdmDisplay && m_tdmStates.size() >= (gch + 1) * kTdmPhaseCount) {
                const QPair<int, int> pair = tdmLocalElePair(m_pair02);
                const int upperSlot = pair.first;
                const int lowerSlot = pair.second;
                const QRectF upper = r.adjusted(0.0, 0.0, 0.0, -r.height() * 0.5);
                const QRectF lower = r.adjusted(0.0, r.height() * 0.5, 0.0, 0.0);

                p.setPen(QPen(borderColor.darker(115), 0.7));
                p.setBrush(colorForState(m_tdmStates[gch * kTdmPhaseCount + upperSlot], warning, error));
                p.drawRoundedRect(upper, 2.0, 2.0);
                p.setBrush(colorForState(m_tdmStates[gch * kTdmPhaseCount + lowerSlot], warning, error));
                p.drawRoundedRect(lower, 2.0, 2.0);
                p.setPen(QPen(borderColor.lighter(110), 0.45));
                p.drawLine(QPointF(r.left() + 1.0, r.center().y()), QPointF(r.right() - 1.0, r.center().y()));
            } else {
                p.setPen(QPen(borderColor.darker(115), 0.7));
                p.setBrush(colorForState(m_states[gch], warning, error));
                p.drawRoundedRect(r, 2.0, 2.0);
            }

            if (m_selectedChannels.contains(gch)) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(primary, 1.5));
                p.drawRoundedRect(r.adjusted(1.0, 1.0, -1.0, -1.0), 2.0, 2.0);
            }
        }
    }

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(borderColor.lighter(115), 0.8));
    for (int col = 1; col < kPhysicalCols; ++col) {
        const double x = mr.left() + col * cellW;
        p.drawLine(QPointF(x, mr.top()), QPointF(x, mr.bottom()));
    }
    for (int blockRow = 1; blockRow < kBlocksPerPhysicalCol; ++blockRow) {
        const double y = mr.top() + blockRow * kLocalsPerBlock * cellH;
        p.drawLine(QPointF(mr.left(), y), QPointF(mr.right(), y));
    }

    p.setPen(axisColor);
    QFont labelFont = font();
    labelFont.setPixelSize(9);
    p.setFont(labelFont);
    p.drawText(QRectF(mr.left(), mr.top() - 15, mr.width(), 12),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("%1 chip order · %2")
                   .arg(m_tdmDisplay ? QStringLiteral("TDM") : QStringLiteral("ADC"))
                   .arg(metricName()));
    p.drawText(QRectF(mr.left(), mr.top() - 15, mr.width(), 12),
               Qt::AlignRight | Qt::AlignVCenter,
               m_tdmDisplay ? QStringLiteral("upper/lower = selected phase pair") : QStringLiteral("1 cell = 1 ADC"));

    const QRectF legendRect(mr.left(), mr.bottom() + 14.0, mr.width() * 0.52, 8.0);
    if (m_metricMode == MetricSaturation) {
        const QRectF okRect(legendRect.left(), legendRect.top(), 18.0, legendRect.height());
        const QRectF satRect(okRect.right() + 8.0, legendRect.top(), 18.0, legendRect.height());
        const QRectF lossRect(satRect.right() + 8.0, legendRect.top(), 18.0, legendRect.height());
        p.setPen(Qt::NoPen);
        p.setBrush(activityColor(0.0));
        p.drawRoundedRect(okRect, 2.0, 2.0);
        p.setBrush(warning);
        p.drawRoundedRect(satRect, 2.0, 2.0);
        p.setBrush(error);
        p.drawRoundedRect(lossRect, 2.0, 2.0);
    } else {
        QLinearGradient gradient(legendRect.topLeft(), legendRect.topRight());
        gradient.setColorAt(0.0, activityColor(0.0));
        gradient.setColorAt(0.55, activityColor(0.55));
        gradient.setColorAt(1.0, activityColor(1.0));
        p.setPen(QPen(borderColor, 0.8));
        p.setBrush(gradient);
        p.drawRoundedRect(legendRect, 3.0, 3.0);
    }

    p.setPen(axisColor);
    p.drawText(QRectF(legendRect.left(), legendRect.bottom() + 4.0, mr.width(), 12.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               metricScaleText());
    p.drawText(QRectF(mr.left(), mr.bottom() + 14.0, mr.width(), 12.0),
               Qt::AlignRight | Qt::AlignVCenter,
               m_tdmDisplay
                   ? (m_pair02
                          ? QStringLiteral("upper/lower = phase 0/2")
                          : QStringLiteral("upper/lower = phase 1/3"))
                   : QStringLiteral("yellow=sat  red=loss"));
}

void ActivityMapView::mousePressEvent(QMouseEvent *event)
{
    const int ch = channelAt(event->position());
    if (ch >= 0) {
        emit channelClicked(ch);
    }
}

} // namespace ccv2
