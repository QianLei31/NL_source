#pragma once
#include <QObject>
#include <QVector>
#include <QSet>
#include "src/core/channel_info.h"

namespace ccv2 {

class ChannelMapModel : public QObject {
    Q_OBJECT
public:
    explicit ChannelMapModel(QObject *parent = nullptr);

    void build();

    // Forward / reverse queries
    const ChannelInfo &byElectrode(int electrodeId) const;
    const ChannelInfo &byGlobalChannel(int globalChannel) const;
    int globalToElectrodeRepresentative(int globalChannel) const;

    // Selection
    void setSelectedElectrodes(const QSet<int> &electrodeIds);
    QSet<int> selectedElectrodes() const;
    QVector<int> selectedGlobalChannels() const;
    QVector<ChannelInfo> selectedChannelInfos() const;

    // Metric updaters
    void updateChannelMetrics(int globalChannel, double rms, double p2p, bool saturated, bool packetLoss);
    void updateStimActive(int globalChannel, bool active);

signals:
    void mapBuilt();
    void selectionChanged();
    void channelInfoChanged(int electrodeId);

private:
    QVector<ChannelInfo> m_byElectrode;       // size = 1024
    QVector<int> m_electrodeOfGlobal;          // size = 256 (representative electrode per global ch)
    QSet<int> m_selectedElectrodes;
};

} // namespace ccv2
