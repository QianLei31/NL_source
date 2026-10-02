#pragma once
#include "page_base.h"

namespace ccv2 {

class ChannelMapModel;
class ElectrodeOverviewWidget;
class InspectorPanel;

class ChannelMapPage : public PageBase {
    Q_OBJECT
public:
    explicit ChannelMapPage(ChannelMapModel *model, QWidget *parent = nullptr);
    void onThemeChanged(const ThemePalette &palette) override;

private:
    ChannelMapModel *m_model = nullptr;
    ElectrodeOverviewWidget *m_mapView = nullptr;
    InspectorPanel *m_inspector = nullptr;
};

} // namespace ccv2
