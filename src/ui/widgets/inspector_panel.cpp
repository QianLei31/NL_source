#include "inspector_panel.h"
#include <QFormLayout>
#include <QStackedLayout>

namespace ccv2 {

InspectorPanel::InspectorPanel(QWidget *parent)
    : QWidget(parent)
{
    setProperty("role", "inspector");
    setMinimumWidth(220);
    setMaximumWidth(280);

    auto *stack = new QStackedLayout(this);

    // Placeholder
    m_placeholder = new QLabel(QStringLiteral("No channel selected.\nClick any electrode on the map."), this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setStyleSheet(QStringLiteral("color: #5d738e; font-size: 12px;"));
    stack->addWidget(m_placeholder);

    // Content
    m_content = new QWidget(this);
    auto *form = new QFormLayout(m_content);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    auto makeLabel = [this]() {
        auto *l = new QLabel(QStringLiteral("--"), m_content);
        l->setProperty("class", "metric-value");
        l->setStyleSheet(QStringLiteral("font-size: 14px;"));
        return l;
    };

    m_globalCh = makeLabel();
    m_block = makeLabel();
    m_localCh = makeLabel();
    m_electrode = makeLabel();
    m_adc = makeLabel();
    m_stim = makeLabel();
    m_spiAddr = makeLabel();
    m_dataLane = makeLabel();

    form->addRow(QStringLiteral("Global CH"), m_globalCh);
    form->addRow(QStringLiteral("Block"), m_block);
    form->addRow(QStringLiteral("Local CH"), m_localCh);
    form->addRow(QStringLiteral("Electrode"), m_electrode);
    form->addRow(QStringLiteral("ADC"), m_adc);
    form->addRow(QStringLiteral("Stimulator"), m_stim);
    form->addRow(QStringLiteral("SPI Addr"), m_spiAddr);
    form->addRow(QStringLiteral("Data Lane"), m_dataLane);

    stack->addWidget(m_content);
    stack->setCurrentWidget(m_placeholder);
}

void InspectorPanel::showElectrode(const ChannelInfo &info)
{
    if (info.electrodeId < 0) {
        clear();
        return;
    }
    m_globalCh->setText(QString::number(info.globalChannel));
    m_block->setText(QString::number(info.blockId));
    m_localCh->setText(QString::number(info.localChannel));
    m_electrode->setText(QStringLiteral("E%1").arg(info.electrodeId, 4, 10, QLatin1Char('0')));
    m_adc->setText(QStringLiteral("ADC%1").arg(info.adcId));
    m_stim->setText(QStringLiteral("STIM%1").arg(info.stimId));
    m_spiAddr->setText(QStringLiteral("0x%1").arg(info.spiAddr, 2, 16, QLatin1Char('0')).toUpper());
    m_dataLane->setText(QStringLiteral("Lane %1").arg(info.dataLane));

    auto *stack = qobject_cast<QStackedLayout*>(layout());
    if (stack) stack->setCurrentWidget(m_content);
}

void InspectorPanel::clear()
{
    auto *stack = qobject_cast<QStackedLayout*>(layout());
    if (stack) stack->setCurrentWidget(m_placeholder);
}

void InspectorPanel::setField(QLabel *label, const QString &value)
{
    if (label) label->setText(value);
}

} // namespace ccv2
