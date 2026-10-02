#pragma once

#include <QMap>
#include <QReadWriteLock>
#include <QVector>

#include <memory>

#include "data/ring_buffer.h"

namespace ccv2 {

class DataStore {
public:
    void reset(const QVector<int> &channels,
               int capacity,
               qint64 initialIndex = 0,
               quint64 timelineEpoch = 0);
    void appendChannelValues(int ch, const QVector<qint32> &values);
    bool appendChannelValuesIfEpoch(int ch,
                                    const QVector<qint32> &values,
                                    quint64 timelineEpoch);
    // Advance every channel's global sample index over `frames` samples lost
    // to queue drops, so later appends keep exact absolute numbering (and the
    // TDM phase derived from it). No-op on epoch mismatch.
    bool skipFramesIfEpoch(qint64 frames, quint64 timelineEpoch);
    bool alignFramesIfEpoch(qint64 nextFrame, quint64 timelineEpoch, bool discard = false);

    bool contains(int ch) const;
    RingBuffer *buffer(int ch);
    std::shared_ptr<RingBuffer> sharedBuffer(int ch) const;
    QVector<int> channels() const;

private:
    mutable QReadWriteLock m_lock;
    QMap<int, std::shared_ptr<RingBuffer>> m_buffers;
    quint64 m_timelineEpoch{0};
};

}  // namespace ccv2
