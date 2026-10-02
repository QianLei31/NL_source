#include "channel_map_model.h"
#include <QDebug>

namespace ccv2 {

namespace {

bool isRoutedElectrode(int electrodeId)
{
    if (electrodeId < 0 || electrodeId >= kElectrodeCount) {
        return false;
    }

    const int blockId = electrodeId / kAfePerBlock;
    const int idInBlock = electrodeId % kAfePerBlock;
    const bool routedEven = (blockId % 8) < 4;
    return (idInBlock % 2 == 0) == routedEven;
}

} // namespace

ChannelMapModel::ChannelMapModel(QObject *parent)
    : QObject(parent)
    , m_byElectrode(kElectrodeCount)
    , m_electrodeOfGlobal(kGlobalChannels, -1)
{
}

void ChannelMapModel::build()
{
    for (int e = 0; e < kElectrodeCount; ++e) {
        ChannelInfo &ci = m_byElectrode[e];
        ci.electrodeId = e;
        ci.blockId = e / kAfePerBlock;                    // 0-63
        ci.idInBlock = e % kAfePerBlock;                  // 0-15
        ci.localChannel = ci.idInBlock / kAdcPerBlock;    // 0-3
        ci.globalChannel = ci.blockId * kAdcPerBlock + ci.localChannel; // 0-255
        ci.adcId = ci.globalChannel;
        ci.stimId = ci.globalChannel;
        ci.spiAddr = (ci.blockId << 2) | ci.localChannel;
        ci.dataLane = ci.blockId / 8;
        ci.enabled = isRoutedElectrode(e);
    }

    // Build reverse map: for each globalChannel, store the first routed electrode.
    for (int gch = 0; gch < kGlobalChannels; ++gch) {
        int blockId = gch / kAdcPerBlock;
        int localCh = gch % kAdcPerBlock;
        for (int i = 0; i < kAdcPerBlock; ++i) {
            int electrode = blockId * kAfePerBlock + localCh * kAdcPerBlock + i;
            if (electrode >= 0 && electrode < kElectrodeCount && m_byElectrode[electrode].enabled) {
                m_electrodeOfGlobal[gch] = electrode;
                break;
            }
        }
    }

    emit mapBuilt();
}

const ChannelInfo &ChannelMapModel::byElectrode(int electrodeId) const
{
    if (electrodeId < 0 || electrodeId >= kElectrodeCount) {
        qWarning() << "ChannelMapModel::byElectrode: out of range" << electrodeId;
        return kInvalidChannelInfo;
    }
    return m_byElectrode[electrodeId];
}

const ChannelInfo &ChannelMapModel::byGlobalChannel(int globalChannel) const
{
    if (globalChannel < 0 || globalChannel >= kGlobalChannels) {
        qWarning() << "ChannelMapModel::byGlobalChannel: out of range" << globalChannel;
        return kInvalidChannelInfo;
    }
    int eId = m_electrodeOfGlobal[globalChannel];
    if (eId < 0 || eId >= kElectrodeCount) {
        return kInvalidChannelInfo;
    }
    return m_byElectrode[eId];
}

int ChannelMapModel::globalToElectrodeRepresentative(int globalChannel) const
{
    if (globalChannel < 0 || globalChannel >= kGlobalChannels) return -1;
    return m_electrodeOfGlobal[globalChannel];
}

void ChannelMapModel::setSelectedElectrodes(const QSet<int> &electrodeIds)
{
    QSet<int> routedElectrodes;
    for (int e : electrodeIds) {
        if (e >= 0 && e < kElectrodeCount && m_byElectrode[e].enabled) {
            routedElectrodes.insert(e);
        }
    }

    if (m_selectedElectrodes != routedElectrodes) {
        // Clear old selection flags
        for (int e : m_selectedElectrodes) {
            if (e >= 0 && e < kElectrodeCount)
                m_byElectrode[e].selected = false;
        }
        m_selectedElectrodes = routedElectrodes;
        // Set new selection flags
        for (int e : m_selectedElectrodes) {
            if (e >= 0 && e < kElectrodeCount)
                m_byElectrode[e].selected = true;
        }
        emit selectionChanged();
    }
}

QSet<int> ChannelMapModel::selectedElectrodes() const
{
    return m_selectedElectrodes;
}

QVector<int> ChannelMapModel::selectedGlobalChannels() const
{
    QSet<int> gchs;
    for (int e : m_selectedElectrodes) {
        if (e >= 0 && e < kElectrodeCount && m_byElectrode[e].enabled)
            gchs.insert(m_byElectrode[e].globalChannel);
    }
    return QVector<int>(gchs.begin(), gchs.end());
}

QVector<ChannelInfo> ChannelMapModel::selectedChannelInfos() const
{
    QVector<ChannelInfo> result;
    for (int e : m_selectedElectrodes) {
        if (e >= 0 && e < kElectrodeCount && m_byElectrode[e].enabled)
            result.append(m_byElectrode[e]);
    }
    return result;
}

void ChannelMapModel::updateChannelMetrics(int globalChannel, double rms, double p2p, bool saturated, bool packetLoss)
{
    if (globalChannel < 0 || globalChannel >= kGlobalChannels) return;
    int eId = m_electrodeOfGlobal[globalChannel];
    if (eId < 0) return;

    // Update all 4 electrodes that share this globalChannel
    int blockId = globalChannel / kAdcPerBlock;
    int localCh = globalChannel % kAdcPerBlock;
    for (int i = 0; i < kAdcPerBlock; ++i) {
        int electrode = blockId * kAfePerBlock + localCh * kAdcPerBlock + i;
        if (electrode >= 0 && electrode < kElectrodeCount && m_byElectrode[electrode].enabled) {
            m_byElectrode[electrode].rmsMicroVolt = rms;
            m_byElectrode[electrode].peakToPeak = p2p;
            m_byElectrode[electrode].saturated = saturated;
            m_byElectrode[electrode].packetLoss = packetLoss;
        }
    }
    emit channelInfoChanged(eId);
}

void ChannelMapModel::updateStimActive(int globalChannel, bool active)
{
    if (globalChannel < 0 || globalChannel >= kGlobalChannels) return;
    int eId = m_electrodeOfGlobal[globalChannel];
    if (eId < 0) return;

    int blockId = globalChannel / kAdcPerBlock;
    int localCh = globalChannel % kAdcPerBlock;
    for (int i = 0; i < kAdcPerBlock; ++i) {
        int electrode = blockId * kAfePerBlock + localCh * kAdcPerBlock + i;
        if (electrode >= 0 && electrode < kElectrodeCount && m_byElectrode[electrode].enabled) {
            m_byElectrode[electrode].stimActive = active;
        }
    }
    emit channelInfoChanged(eId);
}

} // namespace ccv2
