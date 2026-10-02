#pragma once
#include "page_base.h"

namespace ccv2 {

class StackWaveformView;
class ActivityMapView;

class ArrayPreviewPage : public PageBase {
    Q_OBJECT
public:
    explicit ArrayPreviewPage(QWidget *parent = nullptr);
    void onThemeChanged(const ThemePalette &palette) override;

private:
    StackWaveformView *m_stackView = nullptr;
    ActivityMapView *m_activityMap = nullptr;
};

} // namespace ccv2
