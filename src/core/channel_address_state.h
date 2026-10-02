#pragma once

#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

namespace ccv2 {

struct AddressRecord {
    int block{0};
    int localChannel{0};
    QString localBits;
    int globalChannel{0};
    QString addressBits;
};

class ChannelAddressState : public QObject {
    Q_OBJECT

public:
    explicit ChannelAddressState(QObject *parent = nullptr);

    void setSelectedPairs(const QSet<QPair<int, int>> &pairs);
    void clear();

    QVector<QPair<int, int>> selectedPairs() const;
    QVector<int> selectedGlobalChannels() const;
    QVector<AddressRecord> selectedAddressRecords() const;

signals:
    void selectionChanged();

private:
    QSet<QPair<int, int>> m_selectedPairs;
};

}  // namespace ccv2
