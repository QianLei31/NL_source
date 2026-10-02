#include "core/channel_address_state.h"

#include <algorithm>

namespace ccv2 {

ChannelAddressState::ChannelAddressState(QObject *parent)
    : QObject(parent) {}

void ChannelAddressState::setSelectedPairs(const QSet<QPair<int, int>> &pairs) {
    QSet<QPair<int, int>> normalized;
    for (const QPair<int, int> &pair : pairs) {
        if (pair.first >= 0 && pair.first < 64 && pair.second >= 0 && pair.second < 4) {
            normalized.insert(pair);
        }
    }

    if (normalized == m_selectedPairs) {
        return;
    }
    m_selectedPairs = normalized;
    emit selectionChanged();
}

void ChannelAddressState::clear() {
    setSelectedPairs({});
}

QVector<QPair<int, int>> ChannelAddressState::selectedPairs() const {
    QVector<QPair<int, int>> out = m_selectedPairs.values().toVector();
    std::sort(out.begin(), out.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) {
        if (a.first == b.first) {
            return a.second < b.second;
        }
        return a.first < b.first;
    });
    return out;
}

QVector<int> ChannelAddressState::selectedGlobalChannels() const {
    QSet<int> channels;
    for (const QPair<int, int> &pair : m_selectedPairs) {
        channels.insert(pair.first * 4 + pair.second);
    }

    QVector<int> out = channels.values().toVector();
    std::sort(out.begin(), out.end());
    return out;
}

QVector<AddressRecord> ChannelAddressState::selectedAddressRecords() const {
    QVector<AddressRecord> out;
    const QVector<QPair<int, int>> pairs = selectedPairs();
    out.reserve(pairs.size());

    for (const QPair<int, int> &pair : pairs) {
        AddressRecord record;
        record.block = pair.first;
        record.localChannel = pair.second;
        record.localBits = QString::number(record.localChannel, 2).rightJustified(2, QLatin1Char('0'));
        record.globalChannel = record.block * 4 + record.localChannel;
        record.addressBits = QString::number(record.block, 2).rightJustified(8, QLatin1Char('0')) + record.localBits;
        out.push_back(record);
    }
    return out;
}

}  // namespace ccv2
