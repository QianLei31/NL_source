#pragma once
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include "src/core/channel_info.h"

namespace ccv2 {

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(QWidget *parent = nullptr);

public slots:
    void showElectrode(const ChannelInfo &info);
    void clear();

private:
    QLabel *m_placeholder = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_globalCh = nullptr;
    QLabel *m_block = nullptr;
    QLabel *m_localCh = nullptr;
    QLabel *m_electrode = nullptr;
    QLabel *m_adc = nullptr;
    QLabel *m_stim = nullptr;
    QLabel *m_spiAddr = nullptr;
    QLabel *m_dataLane = nullptr;

    void setField(QLabel *label, const QString &value);
};

} // namespace ccv2
