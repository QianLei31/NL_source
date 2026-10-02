#pragma once
#include <QWidget>
#include <QVector>
#include <QColor>
#include "src/core/channel_info.h"
#include "src/theme/theme_manager.h"

namespace ccv2 {

class ChannelMapModel;

/// 64-block overview (8x8) + 16-AFE detail region for selected block
class ElectrodeOverviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit ElectrodeOverviewWidget(QWidget *parent = nullptr);

    void setModel(ChannelMapModel *model);
    void setMapPalette(const ThemePalette &palette);
    void setDetailBlock(int blockId);

signals:
    void blockSelected(int blockId);
    void electrodeClicked(int electrodeId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRectF blockRect(int blockIdx) const;
    QRectF afeRect(int afeIdx) const;
    QColor colorForElectrode(const ChannelInfo &ci) const;

    ChannelMapModel *m_model = nullptr;
    ThemePalette m_palette;
    int m_detailBlock = -1;

    // Layout constants
    static constexpr int kOverviewCols = 8;
    static constexpr int kOverviewRows = 8;
    static constexpr int kAfeCols = 4;
    static constexpr int kAfeRows = 4;
    static constexpr int kBlockGap = 3;
    static constexpr int kAfeGap = 2;
};

} // namespace ccv2
