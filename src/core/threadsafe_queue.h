#pragma once

#include <QByteArray>
#include <QMutex>
#include <QQueue>
#include <QWaitCondition>

#include "core/stream_block_info.h"

namespace ccv2 {

// How many bytes of stream payload an item carries, for drop accounting.
// Non-buffer payload types count as zero (their consumers don't track a
// byte-continuous stream).
inline qint64 payloadBytes(const QByteArray &item) { return item.size(); }
template <typename T>
inline qint64 payloadBytes(const T &) { return 0; }

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(int maxSize = 0) : m_maxSize(maxSize) {}

    bool push(const T &item, bool dropOldestWhenFull = false, T *droppedItem = nullptr,
              const StreamBlockInfo &info = {}) {
        QMutexLocker locker(&m_mutex);
        bool dropped = false;
        if (m_maxSize > 0 && m_queue.size() >= m_maxSize) {
            if (dropOldestWhenFull) {
                if (droppedItem) {
                    *droppedItem = m_queue.head().value;
                }
                m_droppedPayloadBytes += payloadBytes(m_queue.head().value);
                m_queue.dequeue();
                dropped = true;
            } else {
                return false;
            }
        }
        m_queue.enqueue({item, info});
        m_cv.wakeOne();
        return !dropped;
    }

    bool pop(T &out, int timeoutMs) {
        return pop(out, timeoutMs, nullptr);
    }

    // Like pop(), but also reports how many payload bytes drop-oldest pushes
    // evicted since the previous successful pop. Evicted items are always the
    // queue head — older than everything still queued — so the reported bytes
    // sit strictly before `out` in the stream. Consumers use this to advance
    // their absolute frame numbering (and thus the TDM slot parity derived
    // from it) exactly across drops instead of silently falling behind.
    bool pop(T &out, int timeoutMs, qint64 *droppedPayloadBytesBefore,
             StreamBlockInfo *info = nullptr) {
        QMutexLocker locker(&m_mutex);
        if (info) *info = {};
        if (droppedPayloadBytesBefore) {
            *droppedPayloadBytesBefore = 0;
        }
        if (m_queue.isEmpty()) {
            if (!m_cv.wait(&m_mutex, timeoutMs)) {
                return false;
            }
            if (m_queue.isEmpty()) {
                return false;
            }
        }
        if (droppedPayloadBytesBefore) {
            *droppedPayloadBytesBefore = m_droppedPayloadBytes;
        }
        // Cleared even for plain pop(): stale drop bytes must never be
        // attributed to a later chunk they don't precede.
        m_droppedPayloadBytes = 0;
        const Entry entry = m_queue.dequeue();
        out = entry.value;
        if (info) *info = entry.info;
        return true;
    }

    void clear() {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
        m_droppedPayloadBytes = 0;
    }

    void wakeAll() {
        QMutexLocker locker(&m_mutex);
        m_cv.wakeAll();
    }

    int size() const {
        QMutexLocker locker(&m_mutex);
        return m_queue.size();
    }

private:
    struct Entry {
        T value;
        StreamBlockInfo info;
    };
    mutable QMutex m_mutex;
    QWaitCondition m_cv;
    QQueue<Entry> m_queue;
    int m_maxSize;
    qint64 m_droppedPayloadBytes{0};
};

}  // namespace ccv2
