#pragma once
#include <QListWidget>
#include <QVector>

namespace ccv2 {

class LeftNavigation : public QListWidget {
    Q_OBJECT
public:
    explicit LeftNavigation(QWidget *parent = nullptr);

    void addPage(const QString &iconText, const QString &title, int pageIndex);
    void setSubStatus(int pageIndex, const QString &text);

signals:
    void pageRequested(int pageIndex);

private:
    struct NavEntry {
        QString icon;
        QString title;
        QString subStatus;
        int pageIndex = 0;
    };
    QVector<NavEntry> m_entries;
    void refreshItem(int row);
};

} // namespace ccv2
