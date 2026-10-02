#include "channel_map_page.h"
#include "src/model/channel_map_model.h"
#include "src/ui/widgets/electrode_map_view.h"
#include "src/ui/widgets/inspector_panel.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace ccv2 {

ChannelMapPage::ChannelMapPage(ChannelMapModel *model, QWidget *parent)
    : PageBase(parent)
    , m_model(model)
{
    setTitle(QStringLiteral("Channel Map"));
    setSubtitle(QStringLiteral("512 routed electrodes / 1024 total · 64 blocks · 256 ADCs · 256 stimulators"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(12);

    // Header
    auto *header = new QWidget(this);
    header->setProperty("role", "page-header");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    auto *titleLabel = new QLabel(QStringLiteral("CHANNEL MAP"), this);
    titleLabel->setObjectName(QStringLiteral("title"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();

    auto *copyBtn = new QPushButton(QStringLiteral("Copy CSV"), this);
    copyBtn->setObjectName(QStringLiteral("ghost"));
    headerLayout->addWidget(copyBtn);
    auto *exportBtn = new QPushButton(QStringLiteral("Export JSON"), this);
    exportBtn->setObjectName(QStringLiteral("ghost"));
    headerLayout->addWidget(exportBtn);
    layout->addWidget(header);

    // Body: Map + Inspector
    auto *body = new QHBoxLayout;
    body->setSpacing(12);

    m_mapView = new ElectrodeOverviewWidget(this);
    m_mapView->setModel(m_model);
    body->addWidget(m_mapView, 1);

    m_inspector = new InspectorPanel(this);
    body->addWidget(m_inspector, 0);

    layout->addLayout(body, 1);

    // Wire electrode click to inspector
    connect(m_mapView, &ElectrodeOverviewWidget::electrodeClicked, this, [this](int eId) {
        m_inspector->showElectrode(m_model->byElectrode(eId));
    });
}

void ChannelMapPage::onThemeChanged(const ThemePalette &palette)
{
    m_mapView->setMapPalette(palette);
}

} // namespace ccv2
