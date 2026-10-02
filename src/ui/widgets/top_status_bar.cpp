#include "core/app_version.h"
#include "top_status_bar.h"
#include "core/channel_routing.h"
#include <QAbstractAnimation>
#include <QGraphicsOpacityEffect>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QStyle>
#include <QtMath>

namespace ccv2 {

namespace {

QIcon makeSettingsIcon()
{
    QPixmap pixmap(36, 36);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(18, 18);

    QPolygonF teeth;
    constexpr int pointCount = 16;
    for (int i = 0; i < pointCount; ++i) {
        const qreal radius = (i % 2 == 0) ? 13.0 : 10.2;
        const qreal angle = qDegreesToRadians(-90.0 + i * (360.0 / pointCount));
        teeth << QPointF(qCos(angle) * radius, qSin(angle) * radius);
    }

    QPainterPath gear;
    gear.addPolygon(teeth);
    gear.closeSubpath();
    QPainterPath center;
    center.addEllipse(QPointF(0, 0), 4.7, 4.7);
    gear = gear.subtracted(center);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xC8, 0xD1, 0xDC));
    painter.drawPath(gear);

    return QIcon(pixmap);
}

}  // namespace

TopStatusBar::TopStatusBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("topStatusBar"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(16);

    m_titleLabel = new QLabel(QString::fromLatin1(kAppDisplayName), this);
    m_titleLabel->setObjectName(QStringLiteral("title"));
    layout->addWidget(m_titleLabel);

    layout->addStretch();

    m_connBadge = new QLabel(QStringLiteral("● 控制未检测"), this);
    m_connBadge->setProperty("class", "status-badge");
    m_connBadge->setProperty("state", "info");
    layout->addWidget(m_connBadge);

    m_endpoint = new QLabel(QStringLiteral("--  控制:--  数据:--"), this);
    m_endpoint->setProperty("role", "data-value");
    layout->addWidget(m_endpoint);

    m_tsChip = new QLabel(QStringLiteral("⏱ 时间戳 --"), this);
    m_tsChip->setObjectName(QStringLiteral("connDot"));
    m_tsChip->setProperty("state", "warn");
    m_tsChip->setToolTip(QStringLiteral("时间戳连续性后台监控"));
    layout->addWidget(m_tsChip);

    // Recording indicator: a compact pill with a softly pulsing red dot and the
    // elapsed time. Deliberately calm (not the error-red alarm style) — the
    // size/path detail lives in the session toolbar and recording panel.
    m_recChip = new QWidget(this);
    m_recChip->setObjectName(QStringLiteral("recChip"));
    m_recChip->setAttribute(Qt::WA_StyledBackground, true);
    m_recChip->setToolTip(QStringLiteral("录制进行中"));
    auto *recLayout = new QHBoxLayout(m_recChip);
    recLayout->setContentsMargins(9, 2, 10, 2);
    recLayout->setSpacing(6);
    m_recDot = new QLabel(QStringLiteral("●"), m_recChip);
    m_recDot->setObjectName(QStringLiteral("recDot"));
    m_recText = new QLabel(QStringLiteral("REC"), m_recChip);
    m_recText->setObjectName(QStringLiteral("recText"));
    recLayout->addWidget(m_recDot);
    recLayout->addWidget(m_recText);
    m_recChip->setVisible(false);

    // Breathe the dot's opacity so the indicator reads as "live" at a glance.
    auto *dotOpacity = new QGraphicsOpacityEffect(m_recDot);
    dotOpacity->setOpacity(1.0);
    m_recDot->setGraphicsEffect(dotOpacity);
    m_recPulse = new QPropertyAnimation(dotOpacity, "opacity", this);
    m_recPulse->setDuration(1100);
    m_recPulse->setKeyValueAt(0.0, 1.0);
    m_recPulse->setKeyValueAt(0.5, 0.28);
    m_recPulse->setKeyValueAt(1.0, 1.0);
    m_recPulse->setEasingCurve(QEasingCurve::InOutSine);
    m_recPulse->setLoopCount(-1);
    layout->addWidget(m_recChip);

    m_settingsBtn = new QPushButton(this);
    m_settingsBtn->setObjectName(QStringLiteral("settingsIconButton"));
    m_settingsBtn->setIcon(makeSettingsIcon());
    m_settingsBtn->setIconSize(QSize(18, 18));
    m_settingsBtn->setToolTip(QStringLiteral("设置"));
    m_settingsBtn->setAccessibleName(QStringLiteral("设置"));
    m_settingsBtn->setFixedSize(30, 30);
    connect(m_settingsBtn, &QPushButton::clicked, this, &TopStatusBar::settingsRequested);
    layout->addWidget(m_settingsBtn);

    setFixedHeight(40);
}

void TopStatusBar::updateFromStatus(const SystemStatus &status)
{
    // This badge describes the control/SPI endpoint only. Acquisition has its
    // own state beside the receive button and must not overwrite this badge.
    switch (status.controlLink) {
    case ControlLinkState::Reachable:
        m_connBadge->setText(QStringLiteral("● 控制可用"));
        m_connBadge->setProperty("state", "ok");
        break;
    case ControlLinkState::Checking:
        m_connBadge->setText(QStringLiteral("● 控制检测中"));
        m_connBadge->setProperty("state", "warn");
        break;
    case ControlLinkState::Degraded:
        m_connBadge->setText(QStringLiteral("● 控制可用 · 回包异常"));
        m_connBadge->setProperty("state", "warn");
        break;
    case ControlLinkState::Error:
        m_connBadge->setText(QStringLiteral("● 控制异常"));
        m_connBadge->setProperty("state", "error");
        break;
    default:
        m_connBadge->setText(QStringLiteral("● 控制未检测"));
        m_connBadge->setProperty("state", "info");
        break;
    }
    m_connBadge->style()->unpolish(m_connBadge);
    m_connBadge->style()->polish(m_connBadge);

    // Ports are noise on the bar — only the IP stays visible, rest in tooltip.
    m_endpoint->setText(status.host);
    QString tip = QStringLiteral("%1\n控制端口: %2\n数据端口: %3")
                      .arg(status.host).arg(status.port).arg(status.dataPort);
    if (!status.controlDetail.isEmpty()) {
        tip += QStringLiteral("\n控制状态: %1").arg(status.controlDetail);
    }
    if (!status.acquisitionDetail.isEmpty()) {
        tip += QStringLiteral("\n采集状态: %1").arg(status.acquisitionDetail);
    }
    m_connBadge->setToolTip(tip);
    m_endpoint->setToolTip(tip);
}

void TopStatusBar::setTimestampHealth(const TimestampContinuityStats &stats,
                                      bool sessionActive,
                                      bool tdmActive)
{
    if (!m_tsChip) return;
    const char *state = "warn";
    QString text;
    // A loss not divisible by four rotates every subsequent TDM phase. Only
    // meaningful in TDM mode and
    // once the counter is real (estimatedMissingFrames stays 0 on the current
    // all-zero-timestamp firmware, so this never false-alarms there).
    const bool tdmParitySwap =
        tdmActive && (stats.estimatedMissingFrames % kTdmPhaseCount) != 0;
    if (!sessionActive || !stats.hasData) {
        state = "warn";
        text = QStringLiteral("⏱ 时间戳 待机");
    } else if (!stats.calibrated) {
        state = "warn";
        text = QStringLiteral("⏱ 校准中…");
    } else if (tdmParitySwap) {
        state = "error";
        text = QStringLiteral("⚠ TDM相位旋转 · 丢帧%1")
                   .arg(stats.estimatedMissingFrames);
    } else {
        const qint64 anomalies = stats.discontinuities + stats.intraFrameMismatchFrames;
        if (anomalies == 0 && stats.estimatedMissingFrames == 0) {
            state = "ok";
            text = QStringLiteral("⏱ 连续 · %1 帧").arg(stats.framesSeen);
        } else {
            state = "error";
            text = QStringLiteral("⚠ 丢帧%1 · 跳变%2")
                       .arg(stats.estimatedMissingFrames)
                       .arg(stats.discontinuities);
        }
    }
    m_tsChip->setText(text);
    m_tsChip->setProperty("state", state);
    m_tsChip->style()->unpolish(m_tsChip);
    m_tsChip->style()->polish(m_tsChip);

    m_tsChip->setToolTip(
        QStringLiteral("时间戳连续性（后台监控）\n"
                       "帧: %1  跳变: %2  估计丢帧: %3\n"
                       "重复帧: %4  不规则跳变: %5\n"
                       "帧内不一致帧: %6  期望步进: %7\n"
                       "首个错误帧: %8")
            .arg(stats.framesSeen)
            .arg(stats.discontinuities)
            .arg(stats.estimatedMissingFrames)
            .arg(stats.repeatedFrames)
            .arg(stats.irregularJumps)
            .arg(stats.intraFrameMismatchFrames)
            .arg(stats.expectedStep)
            .arg(stats.firstErrorFrame));
}

void TopStatusBar::setRecordingState(bool recording, const QString &text)
{
    if (!m_recChip) return;
    m_recChip->setVisible(recording);
    if (recording) {
        // Show only the "REC mm:ss" head; the size and path are already in the
        // session toolbar and recording panel, so the top-bar chip stays tidy.
        QString label = text.section(QStringLiteral(" / "), 0, 0).trimmed();
        if (label.isEmpty()) {
            label = QStringLiteral("REC");
        }
        if (m_recText) {
            m_recText->setText(label);
        }
        // setRecordingState is called on every timer tick; only (re)start the
        // pulse if it isn't already running so it doesn't reset each second.
        if (m_recPulse && m_recPulse->state() != QAbstractAnimation::Running) {
            m_recPulse->start();
        }
    } else if (m_recPulse) {
        m_recPulse->stop();
    }
}

} // namespace ccv2
