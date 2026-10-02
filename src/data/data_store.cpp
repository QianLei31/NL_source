#include "data/data_store.h"

namespace ccv2 {

void DataStore::reset(const QVector<int> &channels,
                      int capacity,
                      qint64 initialIndex,
                      quint64 timelineEpoch) {
    QWriteLocker locker(&m_lock);
    m_buffers.clear();
    m_timelineEpoch = timelineEpoch;
    for (int ch : channels) {
        m_buffers.insert(ch, std::make_shared<RingBuffer>(capacity, initialIndex));
    }
}

void DataStore::appendChannelValues(int ch, const QVector<qint32> &values) {
    std::shared_ptr<RingBuffer> target;
    {
        QReadLocker locker(&m_lock);
        const auto it = m_buffers.constFind(ch);
        if (it == m_buffers.cend()) {
            return;
        }
        target = it.value();
    }
    target->appendMany(values);
}

bool DataStore::appendChannelValuesIfEpoch(int ch,
                                           const QVector<qint32> &values,
                                           quint64 timelineEpoch) {
    std::shared_ptr<RingBuffer> target;
    {
        QReadLocker locker(&m_lock);
        if (m_timelineEpoch != timelineEpoch) {
            return false;
        }
        const auto it = m_buffers.constFind(ch);
        if (it == m_buffers.cend()) {
            return false;
        }
        target = it.value();
        // Keep the read lock while appending so reset cannot replace the
        // timeline between the epoch check and the write.
        target->appendMany(values);
    }
    return true;
}

bool DataStore::skipFramesIfEpoch(qint64 frames, quint64 timelineEpoch) {
    if (frames <= 0) {
        return true;
    }
    QReadLocker locker(&m_lock);
    if (m_timelineEpoch != timelineEpoch) {
        return false;
    }
    for (auto it = m_buffers.cbegin(); it != m_buffers.cend(); ++it) {
        it.value()->skipMany(frames);
    }
    return true;
}

bool DataStore::contains(int ch) const {
    QReadLocker locker(&m_lock);
    return m_buffers.contains(ch);
}

bool DataStore::alignFramesIfEpoch(qint64 nextFrame, quint64 epoch, bool discard) {
    QReadLocker locker(&m_lock);
    if (m_timelineEpoch != epoch) return false;
    for (const auto &buffer : m_buffers) buffer->alignNextIndex(nextFrame, discard);
    return true;
}

RingBuffer *DataStore::buffer(int ch) {
    return sharedBuffer(ch).get();
}

std::shared_ptr<RingBuffer> DataStore::sharedBuffer(int ch) const {
    QReadLocker locker(&m_lock);
    const auto it = m_buffers.constFind(ch);
    return it == m_buffers.cend() ? std::shared_ptr<RingBuffer>() : it.value();
}

QVector<int> DataStore::channels() const {
    QReadLocker locker(&m_lock);
    return m_buffers.keys().toVector();
}

}  // namespace ccv2
