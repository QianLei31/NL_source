#include "ui/widgets/channel_grid_widget.h"

#include <algorithm>

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

namespace ccv2 {

namespace {

QColor colorFromPalette(const QMap<QString, QString> &palette,
                        const QString &key,
                        const QColor &fallback)
{
    const QColor color(palette.value(key));
    return color.isValid() ? color : fallback;
}

QColor blend(const QColor &a, const QColor &b, double ratio)
{
    const double r = qBound(0.0, ratio, 1.0);
    const double ar = 1.0 - r;
    return QColor(static_cast<int>(a.red() * ar + b.red() * r + 0.5),
                  static_cast<int>(a.green() * ar + b.green() * r + 0.5),
                  static_cast<int>(a.blue() * ar + b.blue() * r + 0.5));
}

}  // namespace

ChannelGridWidget::ChannelGridWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setChannelCount(256, 4);
}

void ChannelGridWidget::setChannelCount(int totalChannels, int localChannelsPerBlock)
{
    m_totalChannels = qMax(1, totalChannels);
    m_localChannelsPerBlock = qMax(1, localChannelsPerBlock);
    m_state = QVector<int>(m_totalChannels, 0);
    m_selected.clear();
    updateGeometry();
    resize(sizeHint());
    update();
}

void ChannelGridWidget::setSelectedChannels(const QSet<int> &channels)
{
    QSet<int> normalized;
    for (int channel : channels) {
        if (channel >= 0 && channel < m_totalChannels) {
            normalized.insert(channel);
        }
    }
    if (m_selected == normalized) {
        return;
    }
    m_selected = normalized;
    update();
}

void ChannelGridWidget::clearSelection()
{
    emitSelectionIfChanged({});
}

void ChannelGridWidget::setChannelState(int channel, int state)
{
    if (channel < 0 || channel >= m_state.size()) {
        return;
    }
    m_state[channel] = qBound(0, state, 2);
    update(cellRect(channel).adjusted(-2, -2, 2, 2).toAlignedRect());
}

void ChannelGridWidget::setThemePalette(const QMap<QString, QString> &palette)
{
    m_bgColor = colorFromPalette(palette, QStringLiteral("bg"), m_bgColor);
    m_blockBg = colorFromPalette(palette, QStringLiteral("block_deep"), m_blockBg);
    m_cellIdle = colorFromPalette(palette, QStringLiteral("electrode"), m_cellIdle);
    m_cellSelected = colorFromPalette(palette, QStringLiteral("selected"), m_cellSelected);
    m_border = colorFromPalette(palette, QStringLiteral("outline"), m_border);
    m_text = colorFromPalette(palette, QStringLiteral("block_label_deep"), m_text);
    m_cellHover = blend(m_cellIdle, m_cellSelected, 0.18);
    m_cellActive = colorFromPalette(palette, QStringLiteral("active"), m_cellActive);
    m_cellDisabled = blend(m_bgColor, m_cellIdle, 0.35);
    update();
}

void ChannelGridWidget::zoomIn()
{
    setZoomPercent(zoomPercent() + 10);
}

void ChannelGridWidget::zoomOut()
{
    setZoomPercent(zoomPercent() - 10);
}

void ChannelGridWidget::resetZoom()
{
    setZoomPercent(100);
}

void ChannelGridWidget::setZoomPercent(int percent)
{
    const qreal next = qBound(65, percent, 250) / 100.0;
    if (qFuzzyCompare(m_zoom, next)) {
        return;
    }
    m_zoom = next;
    updateGeometry();
    resize(sizeHint());
    update();
    emit zoomChanged(zoomPercent());
}

QRectF ChannelGridWidget::blockRect(int block) const
{
    const int blockCount = (m_totalChannels + m_localChannelsPerBlock - 1) / m_localChannelsPerBlock;
    if (block < 0 || block >= blockCount) {
        return {};
    }

    const int localRows = (m_localChannelsPerBlock + m_layout.localColumns - 1) / m_layout.localColumns;
    const qreal cell = m_layout.cellSize * m_zoom;
    const qreal cellGap = m_layout.cellGap * m_zoom;
    const qreal blockGap = m_layout.blockGap * m_zoom;
    const qreal labelH = m_layout.blockLabelHeight * m_zoom;
    const qreal pad = m_layout.outerPad * m_zoom;

    const qreal blockW = m_layout.localColumns * cell + (m_layout.localColumns - 1) * cellGap + pad;
    const qreal blockH = labelH + localRows * cell + (localRows - 1) * cellGap + pad;

    const int blockCol = block % m_layout.blockColumns;
    const int blockRow = block / m_layout.blockColumns;
    const qreal x = pad + blockCol * (blockW + blockGap);
    const qreal y = pad + blockRow * (blockH + blockGap);
    return QRectF(x, y, blockW, blockH);
}

QRectF ChannelGridWidget::cellRect(int channel) const
{
    if (channel < 0 || channel >= m_totalChannels) {
        return {};
    }
    const int block = channel / m_localChannelsPerBlock;
    const int local = channel % m_localChannelsPerBlock;
    const QRectF br = blockRect(block);

    const qreal cell = m_layout.cellSize * m_zoom;
    const qreal cellGap = m_layout.cellGap * m_zoom;
    const qreal labelH = m_layout.blockLabelHeight * m_zoom;
    const qreal pad = m_layout.outerPad * m_zoom / 2.0;

    const int localCol = local % m_layout.localColumns;
    const int localRow = local / m_layout.localColumns;
    const qreal x = br.left() + pad + localCol * (cell + cellGap);
    const qreal y = br.top() + labelH + localRow * (cell + cellGap);
    return QRectF(x, y, cell, cell);
}

int ChannelGridWidget::channelAt(const QPoint &pos) const
{
    for (int channel = 0; channel < m_totalChannels; ++channel) {
        if (cellRect(channel).contains(pos)) {
            return channel;
        }
    }
    return -1;
}

QString ChannelGridWidget::channelTooltip(int channel) const
{
    if (channel < 0) {
        return {};
    }
    const int block = channel / m_localChannelsPerBlock;
    const int local = channel % m_localChannelsPerBlock;
    const QString localBits = QString::number(local, 2).rightJustified(2, QLatin1Char('0'));
    const QString addr = QString::number(block, 2).rightJustified(8, QLatin1Char('0')) + localBits;
    return QStringLiteral("Block %1 | Local %2 | CH %3 | SPI %4")
        .arg(block, 2, 10, QLatin1Char('0'))
        .arg(localBits)
        .arg(channel, 3, 10, QLatin1Char('0'))
        .arg(addr);
}

QSize ChannelGridWidget::sizeHint() const
{
    const int blockCount = (m_totalChannels + m_localChannelsPerBlock - 1) / m_localChannelsPerBlock;
    if (blockCount <= 0) {
        return QSize(320, 240);
    }
    const QRectF last = blockRect(blockCount - 1);
    const qreal pad = m_layout.outerPad * m_zoom;
    return QSize(qCeil(last.right() + pad), qCeil(last.bottom() + pad));
}

void ChannelGridWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), m_bgColor);

    const int blockCount = (m_totalChannels + m_localChannelsPerBlock - 1) / m_localChannelsPerBlock;
    QFont blockFont = painter.font();
    blockFont.setPointSizeF(qMax(7.0, 8.0 * m_zoom));
    blockFont.setBold(true);

    for (int block = 0; block < blockCount; ++block) {
        const QRectF br = blockRect(block);
        if (!br.intersects(rect())) {
            continue;
        }

        QColor bg = m_blockBg;
        bg.setAlpha((block / m_layout.blockColumns) % 2 == 0 ? 190 : 150);
        painter.setPen(QPen(m_border, 1.0));
        painter.setBrush(bg);
        painter.drawRoundedRect(br, 5.0, 5.0);

        painter.setFont(blockFont);
        painter.setPen(m_text);
        painter.drawText(br.adjusted(5.0, 1.0, -5.0, 0.0),
                         Qt::AlignTop | Qt::AlignHCenter,
                         QStringLiteral("B%1").arg(block, 2, 10, QLatin1Char('0')));
    }

    for (int channel = 0; channel < m_totalChannels; ++channel) {
        const QRectF cr = cellRect(channel);
        if (!cr.intersects(rect())) {
            continue;
        }

        QColor fill = m_cellIdle;
        if (m_state.value(channel) == 2) {
            fill = m_cellDisabled;
        } else if (m_selected.contains(channel)) {
            fill = m_cellSelected;
        } else if (m_state.value(channel) == 1) {
            fill = m_cellActive;
        } else if (channel == m_hoverChannel) {
            fill = m_cellHover;
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(cr, 3.0, 3.0);

        if (cr.width() >= 20.0) {
            QColor labelColor = m_selected.contains(channel) ? QColor(QStringLiteral("#0B1020")) : m_text;
            labelColor.setAlpha(210);
            painter.setPen(labelColor);
            QFont cellFont = painter.font();
            cellFont.setPointSizeF(qMax(6.0, 7.0 * m_zoom));
            cellFont.setBold(false);
            painter.setFont(cellFont);
            painter.drawText(cr, Qt::AlignCenter, QString::number(channel % m_localChannelsPerBlock));
        }
    }

    if (m_dragging) {
        const QRect box = QRect(m_dragStart, m_dragCurrent).normalized();
        QColor fill = m_cellSelected;
        fill.setAlpha(34);
        painter.setPen(QPen(m_cellSelected, 1.0));
        painter.setBrush(fill);
        painter.drawRect(box);
    }
}

void ChannelGridWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_ctrlHeld = event->modifiers() & Qt::ControlModifier;
    m_dragging = true;
    m_dragStart = event->pos();
    m_dragCurrent = event->pos();

    const int channel = channelAt(event->pos());
    if (channel >= 0) {
        QSet<int> next = m_ctrlHeld ? m_selected : QSet<int>{};
        if (m_ctrlHeld && next.contains(channel)) {
            next.remove(channel);
        } else {
            next.insert(channel);
        }
        emitSelectionIfChanged(next);
        emit channelActivated(channel);
    }
}

void ChannelGridWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int channel = channelAt(event->pos());
    if (channel != m_hoverChannel) {
        m_hoverChannel = channel;
        emit channelHovered(channel);
        update();
    }

    if (channel >= 0) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QToolTip::showText(event->globalPosition().toPoint(), channelTooltip(channel), this);
#else
        QToolTip::showText(event->globalPos(), channelTooltip(channel), this);
#endif
    }

    if (m_dragging) {
        m_dragCurrent = event->pos();
        const QRect box = QRect(m_dragStart, m_dragCurrent).normalized();
        if (box.width() > 3 || box.height() > 3) {
            applyDragSelection(box);
        }
    }
}

void ChannelGridWidget::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    update();
}

void ChannelGridWidget::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() > 0) {
            zoomIn();
        } else {
            zoomOut();
        }
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void ChannelGridWidget::leaveEvent(QEvent *)
{
    if (m_hoverChannel != -1) {
        m_hoverChannel = -1;
        emit channelHovered(-1);
        update();
    }
}

void ChannelGridWidget::applyDragSelection(const QRect &box)
{
    QSet<int> next = m_ctrlHeld ? m_selected : QSet<int>{};
    for (int channel = 0; channel < m_totalChannels; ++channel) {
        if (cellRect(channel).intersects(box)) {
            next.insert(channel);
        }
    }
    emitSelectionIfChanged(next);
}

void ChannelGridWidget::emitSelectionIfChanged(const QSet<int> &next)
{
    if (next == m_selected) {
        return;
    }
    m_selected = next;
    emit selectionChanged(m_selected);
    update();
}

}  // namespace ccv2
