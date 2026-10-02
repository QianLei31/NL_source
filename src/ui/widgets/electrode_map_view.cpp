#include "electrode_map_view.h"
#include "src/model/channel_map_model.h"
#include <QPainter>
#include <QMouseEvent>
#include <cmath>

namespace ccv2 {

ElectrodeOverviewWidget::ElectrodeOverviewWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(400, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ElectrodeOverviewWidget::setModel(ChannelMapModel *model)
{
    m_model = model;
    update();
}

void ElectrodeOverviewWidget::setMapPalette(const ThemePalette &palette)
{
    m_palette = palette;
    update();
}

void ElectrodeOverviewWidget::setDetailBlock(int blockId)
{
    if (m_detailBlock != blockId) {
        m_detailBlock = blockId;
        update();
    }
}

void ElectrodeOverviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background
    p.fillRect(rect(), m_palette.plotBg.isValid() ? m_palette.plotBg : QColor(0x08, 0x15, 0x24));

    if (!m_model) {
        p.setPen(m_palette.textDisabled.isValid() ? m_palette.textDisabled : Qt::gray);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No channel map loaded.\nCall build() first."));
        return;
    }

    // === Draw 8x8 block overview ===
    for (int blockIdx = 0; blockIdx < kBlockCount; ++blockIdx) {
        QRectF r = blockRect(blockIdx);

        // Count selected routed electrodes in this block.
        int selectedCount = 0;
        bool hasError = false;
        for (int afe = 0; afe < kAfePerBlock; ++afe) {
            int eId = blockIdx * kAfePerBlock + afe;
            const auto &ci = m_model->byElectrode(eId);
            if (!ci.enabled) continue;
            if (ci.selected) selectedCount++;
            if (ci.packetLoss || ci.saturated) hasError = true;
        }

        // Block color
        QColor fill = m_palette.cardBg.isValid() ? m_palette.cardBg : QColor(0x0e, 0x22, 0x38);
        if (blockIdx == m_detailBlock)
            fill = m_palette.primary.isValid() ? m_palette.primary.darker(150) : QColor(0x15, 0x65, 0xc0);
        else if (selectedCount > 0)
            fill = m_palette.cardBgElevated.isValid() ? m_palette.cardBgElevated : QColor(0x15, 0x30, 0x4b);

        QColor border = hasError ? (m_palette.error.isValid() ? m_palette.error : Qt::red)
                                 : (m_palette.cardBorder.isValid() ? m_palette.cardBorder : QColor(0x1f, 0x3d, 0x5e));

        p.setPen(QPen(border, 1));
        p.setBrush(fill);
        p.drawRoundedRect(r, 3, 3);

        // Block label
        p.setPen(m_palette.textSecondary.isValid() ? m_palette.textSecondary : Qt::lightGray);
        QFont f = font();
        f.setPixelSize(qMax(8, (int)(r.height() * 0.3)));
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, QString::number(blockIdx));
    }

    // === Draw 4x4 AFE detail for selected block ===
    const int overviewW = width() * 55 / 100;
    const int detailX = overviewW + 10;
    const int detailW = width() - detailX - 5;
    if (m_detailBlock >= 0 && m_detailBlock < kBlockCount) {
        // Title
        p.setPen(m_palette.textPrimary.isValid() ? m_palette.textPrimary : Qt::white);
        QFont tf = font();
        tf.setPixelSize(12);
        tf.setBold(true);
        p.setFont(tf);
        p.drawText(QRectF(detailX, 4, detailW, 24), Qt::AlignCenter,
                   QStringLiteral("Block %1 - routed AFEs").arg(m_detailBlock));

        for (int afe = 0; afe < kAfePerBlock; ++afe) {
            int eId = m_detailBlock * kAfePerBlock + afe;
            const auto &ci = m_model->byElectrode(eId);

            QRectF r = afeRect(afe);

            QColor fill = colorForElectrode(ci);
            QColor border = m_palette.cardBorder.isValid() ? m_palette.cardBorder : QColor(0x1f, 0x3d, 0x5e);

            p.setPen(QPen(border, 1));
            p.setBrush(fill);
            p.drawRoundedRect(r, 4, 4);

            // AFE label
            p.setPen(ci.enabled
                         ? (m_palette.textPrimary.isValid() ? m_palette.textPrimary : Qt::white)
                         : (m_palette.textDisabled.isValid() ? m_palette.textDisabled : Qt::gray));
            QFont sf = font();
            sf.setPixelSize(qMax(8, (int)(r.height() * 0.25)));
            p.setFont(sf);
            p.drawText(r, Qt::AlignCenter, QStringLiteral("E%1\nCH%2").arg(eId).arg(ci.globalChannel));
        }
    } else {
        // No block selected placeholder
        p.setPen(m_palette.textDisabled.isValid() ? m_palette.textDisabled : Qt::gray);
        p.drawText(QRectF(detailX, 0, detailW, height()), Qt::AlignCenter,
                   QStringLiteral("Click a block\nto see detail"));
    }
}

void ElectrodeOverviewWidget::mousePressEvent(QMouseEvent *event)
{
    if (!m_model) return;

    QPointF pos = event->position();
    const int overviewW = width() * 55 / 100;

    // Check if click is in overview area
    if (pos.x() < overviewW) {
        for (int blockIdx = 0; blockIdx < kBlockCount; ++blockIdx) {
            if (!blockRect(blockIdx).contains(pos)) {
                continue;
            }
            setDetailBlock(blockIdx);
            emit blockSelected(blockIdx);
            return;
        }
    }

    // Check if click is in detail area
    if (m_detailBlock >= 0) {
        for (int afe = 0; afe < kAfePerBlock; ++afe) {
            if (!afeRect(afe).contains(pos)) {
                continue;
            }
            int eId = m_detailBlock * kAfePerBlock + afe;
            if (m_model->byElectrode(eId).enabled) {
                emit electrodeClicked(eId);
            }
            return;
        }
    }
}

QRectF ElectrodeOverviewWidget::blockRect(int blockIdx) const
{
    if (blockIdx < 0 || blockIdx >= kBlockCount) {
        return QRectF();
    }
    const int overviewW = width() * 55 / 100;
    const double bw = (overviewW - kBlockGap * (kOverviewCols + 1)) / static_cast<double>(kOverviewCols);
    const double bh = (height() - kBlockGap * (kOverviewRows + 1)) / static_cast<double>(kOverviewRows);
    return QRectF(kBlockGap + (blockIdx % kOverviewCols) * (bw + kBlockGap),
                  kBlockGap + (blockIdx / kOverviewCols) * (bh + kBlockGap),
                  bw,
                  bh);
}

QRectF ElectrodeOverviewWidget::afeRect(int afeIdx) const
{
    if (afeIdx < 0 || afeIdx >= kAfePerBlock) {
        return QRectF();
    }
    const int overviewW = width() * 55 / 100;
    const int detailX = overviewW + 10;
    const int detailW = width() - detailX - 5;
    const double aw = (detailW - kAfeGap * (kAfeCols + 1)) / static_cast<double>(kAfeCols);
    const double ah = (height() - kAfeGap * (kAfeRows + 1) - 30) / static_cast<double>(kAfeRows);
    return QRectF(detailX + kAfeGap + (afeIdx % kAfeCols) * (aw + kAfeGap),
                  30 + kAfeGap + (afeIdx / kAfeCols) * (ah + kAfeGap),
                  aw,
                  ah);
}

QColor ElectrodeOverviewWidget::colorForElectrode(const ChannelInfo &ci) const
{
    // Priority: error > stimActive > selected > enabled > disabled
    if (ci.packetLoss || ci.saturated)
        return m_palette.error.isValid() ? m_palette.error : QColor(0xe9, 0x45, 0x45);
    if (ci.stimActive)
        return m_palette.warning.isValid() ? m_palette.warning : QColor(0xf5, 0xa6, 0x23);
    if (ci.selected)
        return m_palette.primary.isValid() ? m_palette.primary : QColor(0x1e, 0x88, 0xe5);
    if (ci.enabled)
        return m_palette.cardBgElevated.isValid() ? m_palette.cardBgElevated : QColor(0x15, 0x30, 0x4b);
    // disabled
    return m_palette.cardBg.isValid() ? m_palette.cardBg : QColor(0x0e, 0x22, 0x38);
}

} // namespace ccv2
