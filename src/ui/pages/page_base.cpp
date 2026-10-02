#include "page_base.h"

namespace ccv2 {

PageBase::PageBase(QWidget *parent)
    : QWidget(parent)
{
}

void PageBase::setTitle(const QString &title)
{
    if (m_title != title) {
        m_title = title;
        emit metaChanged();
    }
}

void PageBase::setSubtitle(const QString &subtitle)
{
    if (m_subtitle != subtitle) {
        m_subtitle = subtitle;
        emit metaChanged();
    }
}

} // namespace ccv2
