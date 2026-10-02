#include "left_navigation.h"

namespace ccv2 {

LeftNavigation::LeftNavigation(QWidget *parent)
    : QListWidget(parent)
{
    setFixedWidth(220);
    setFrameShape(QFrame::NoFrame);
    connect(this, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < m_entries.size())
            emit pageRequested(m_entries[row].pageIndex);
    });
}

void LeftNavigation::addPage(const QString &iconText, const QString &title, int pageIndex)
{
    NavEntry entry;
    entry.icon = iconText;
    entry.title = title;
    entry.pageIndex = pageIndex;
    m_entries.append(entry);

    addItem(QStringLiteral("%1  %2").arg(iconText, title));
}

void LeftNavigation::setSubStatus(int pageIndex, const QString &text)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].pageIndex == pageIndex) {
            m_entries[i].subStatus = text;
            refreshItem(i);
            break;
        }
    }
}

void LeftNavigation::refreshItem(int row)
{
    if (row < 0 || row >= count()) return;
    const auto &e = m_entries[row];
    QString display = QStringLiteral("%1  %2").arg(e.icon, e.title);
    if (!e.subStatus.isEmpty())
        display += QStringLiteral("\n     %1").arg(e.subStatus);
    item(row)->setText(display);
}

} // namespace ccv2
