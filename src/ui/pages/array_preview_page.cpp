#include "array_preview_page.h"
#include "src/ui/widgets/stack_waveform_view.h"
#include "src/ui/widgets/activity_map_view.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QFrame>

namespace ccv2 {

ArrayPreviewPage::ArrayPreviewPage(QWidget *parent)
    : PageBase(parent)
{
    setTitle(QStringLiteral("Array Preview"));
    setSubtitle(QStringLiteral("Multi-channel downsampled preview · Not for performance measurement"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(12);

    // Header
    auto *header = new QHBoxLayout;
    auto *titleLabel = new QLabel(QStringLiteral("ARRAY PREVIEW"), this);
    titleLabel->setObjectName(QStringLiteral("title"));
    header->addWidget(titleLabel);
    header->addStretch();

    auto *modeCombo = new QComboBox(this);
    modeCombo->addItems({QStringLiteral("Stack View"), QStringLiteral("Tile View"), QStringLiteral("Activity Map")});
    header->addWidget(modeCombo);

    auto *startBtn = new QPushButton(QStringLiteral("▶ Start"), this);
    startBtn->setObjectName(QStringLiteral("primary"));
    header->addWidget(startBtn);

    auto *stopBtn = new QPushButton(QStringLiteral("■ Stop"), this);
    stopBtn->setObjectName(QStringLiteral("danger"));
    header->addWidget(stopBtn);

    layout->addLayout(header);

    // Banner
    auto *banner = new QFrame(this);
    banner->setProperty("role", "banner");
    banner->setProperty("severity", "warning");
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(12, 6, 12, 6);
    auto *bannerLabel = new QLabel(QStringLiteral("⚡ Preview Mode | Downsampled | Not for Performance Measurement"), this);
    bannerLabel->setStyleSheet(QStringLiteral("color: #ffaa00; font-size: 11px; font-weight: 600;"));
    bannerLayout->addWidget(bannerLabel);
    layout->addWidget(banner);

    // Body: Stack view + Activity map
    auto *body = new QHBoxLayout;
    body->setSpacing(12);

    m_stackView = new StackWaveformView(this);
    m_stackView->setPlaceholderText(QStringLiteral("No data stream.\nSelect channels and click Start to begin acquisition."));
    body->addWidget(m_stackView, 3);

    m_activityMap = new ActivityMapView(this);
    m_activityMap->setMinimumWidth(180);
    m_activityMap->setMaximumWidth(240);
    body->addWidget(m_activityMap, 1);

    layout->addLayout(body, 1);
}

void ArrayPreviewPage::onThemeChanged(const ThemePalette &palette)
{
    m_activityMap->setPalette(palette);
}

} // namespace ccv2
