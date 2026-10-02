#include "metric_card.h"
#include <QVBoxLayout>
#include <QStyle>

namespace ccv2 {

MetricCard::MetricCard(const QString &label, const QString &unit, QWidget *parent)
    : QFrame(parent)
{
    setProperty("role", "metric-card");
    setMinimumWidth(110);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(2);

    m_badge = new QLabel(this);
    m_badge->setProperty("class", "status-badge");
    m_badge->setProperty("state", "ok");
    m_badge->hide();
    layout->addWidget(m_badge, 0, Qt::AlignRight);

    m_label = new QLabel(label, this);
    m_label->setProperty("class", "metric-unit");
    layout->addWidget(m_label);

    m_value = new QLabel(QStringLiteral("--"), this);
    m_value->setProperty("class", "metric-value");
    layout->addWidget(m_value);

    m_unit = new QLabel(unit, this);
    m_unit->setProperty("class", "metric-unit");
    layout->addWidget(m_unit);
}

void MetricCard::setValue(double v, int precision)
{
    m_value->setText(QString::number(v, 'f', precision));
}

void MetricCard::setValueText(const QString &text)
{
    m_value->setText(text);
}

void MetricCard::setSeverity(Severity s)
{
    switch (s) {
    case Severity::Warning:
        m_badge->setProperty("state", "warn");
        m_badge->setText(QStringLiteral("!"));
        m_badge->show();
        break;
    case Severity::Error:
        m_badge->setProperty("state", "error");
        m_badge->setText(QStringLiteral("!!"));
        m_badge->show();
        break;
    default:
        m_badge->hide();
        break;
    }
    m_badge->style()->unpolish(m_badge);
    m_badge->style()->polish(m_badge);
}

void MetricCard::setBadge(const QString &text)
{
    m_badge->setText(text);
    m_badge->show();
}

QString MetricCard::valueText() const
{
    return m_value->text();
}

} // namespace ccv2
