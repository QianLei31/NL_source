#pragma once
#include <QWidget>
#include "src/core/system_status.h"

namespace ccv2 {

struct ThemePalette; // forward declare; will be defined in theme_manager.h later

class PageBase : public QWidget {
    Q_OBJECT
public:
    explicit PageBase(QWidget *parent = nullptr);
    ~PageBase() override = default;

    void setTitle(const QString &title);
    void setSubtitle(const QString &subtitle);
    QString title() const { return m_title; }
    QString subtitle() const { return m_subtitle; }

    // Lifecycle hooks called by MainWindow
    virtual void onActivated() {}
    virtual void onDeactivated() {}
    virtual void shutdown() {}
    virtual void onSystemStatusChanged(const SystemStatus &status) { Q_UNUSED(status); }
    virtual void onThemeChanged(const ThemePalette &palette) { Q_UNUSED(palette); }

signals:
    void metaChanged();

protected:
    QString m_title;
    QString m_subtitle;
};

} // namespace ccv2
