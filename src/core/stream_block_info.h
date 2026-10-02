#pragma once

#include <QVector>

namespace ccv2 {

// Immutable source coordinates travel with the bytes through queue eviction
// and timeline resets. Empty indices denote an unstamped, standalone producer.
struct StreamBlockInfo {
    quint64 epoch{0};
    QVector<qint64> frameIndices;

    bool valid() const { return !frameIndices.isEmpty(); }
    qint64 firstFrame() const { return frameIndices.first(); }
    qint64 nextFrame() const { return frameIndices.last() + 1; }
};

}  // namespace ccv2
