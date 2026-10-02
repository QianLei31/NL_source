#pragma once

#include <QColor>
#include <QMap>
#include <QPoint>
#include <QSet>
#include <QVector>
#include <QWidget>

namespace ccv2 {

class ChannelGridWidget : public QWidget {
    Q_OBJECT

public:
    explicit ChannelGridWidget(QWidget *parent = nullptr);

    void setChannelCount(int totalChannels, int localChannelsPerBlock = 4);
    void setSelectedChannels(const QSet<int> &channels);
    QSet<int> selectedChannels() const { return m_selected; }
    void clearSelection();
    void setChannelState(int channel, int state);
    void setThemePalette(const QMap<QString, QString> &palette);

    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setZoomPercent(int percent);
    int zoomPercent() const { return qRound(m_zoom * 100.0); }

    QSize sizeHint() const override;

signals:
    void selectionChanged(const QSet<int> &selected);
    void channelHovered(int channel);
    void channelActivated(int channel);
    void zoomChanged(int zoomPercent);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Layout {
        int blockColumns{8};
        int localColumns{2};
        qreal cellSize{22.0};
        qreal cellGap{4.0};
        qreal blockGap{12.0};
        qreal blockLabelHeight{16.0};
        qreal outerPad{12.0};
    };

    QRectF cellRect(int channel) const;
    QRectF blockRect(int block) const;
    int channelAt(const QPoint &pos) const;
    QString channelTooltip(int channel) const;
    void applyDragSelection(const QRect &box);
    void emitSelectionIfChanged(const QSet<int> &next);

    int m_totalChannels{256};
    int m_localChannelsPerBlock{4};
    Layout m_layout;
    qreal m_zoom{1.0};

    QSet<int> m_selected;
    QVector<int> m_state;
    int m_hoverChannel{-1};

    bool m_dragging{false};
    bool m_ctrlHeld{false};
    QPoint m_dragStart;
    QPoint m_dragCurrent;

    QColor m_bgColor{QStringLiteral("#10161F")};
    QColor m_blockBg{QStringLiteral("#171F2A")};
    QColor m_cellIdle{QStringLiteral("#202B38")};
    QColor m_cellHover{QStringLiteral("#2A3748")};
    QColor m_cellSelected{QStringLiteral("#7AA7FF")};
    QColor m_cellActive{QStringLiteral("#64D39A")};
    QColor m_cellDisabled{QStringLiteral("#111820")};
    QColor m_border{QStringLiteral("#2D3A4A")};
    QColor m_text{QStringLiteral("#B6C2D0")};
};

}  // namespace ccv2
