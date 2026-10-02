#pragma once

#include <QMutex>
#include <QPair>
#include <QVector>

namespace ccv2 {

class RingBuffer {
public:
    explicit RingBuffer(int capacity = 65536, qint64 initialIndex = 0)
        : m_capacity(qMax(1, capacity)),
          m_data(m_capacity, 0),
          m_writeIdx(0),
          m_size(0),
          m_totalWritten(qMax<qint64>(0, initialIndex)) {}

    void appendMany(const QVector<qint32> &values) {
        if (values.isEmpty()) {
            return;
        }

        const qsizetype sourceCount = values.size();
        QVector<qint32> clipped = values;
        if (clipped.size() >= m_capacity) {
            clipped = clipped.sliced(clipped.size() - m_capacity);
        }

        QMutexLocker locker(&m_mutex);
        const int n = clipped.size();
        const int first = qMin(n, m_capacity - m_writeIdx);
        for (int i = 0; i < first; ++i) {
            m_data[m_writeIdx + i] = clipped[i];
        }

        const int remaining = n - first;
        for (int i = 0; i < remaining; ++i) {
            m_data[i] = clipped[first + i];
        }

        m_writeIdx = (m_writeIdx + n) % m_capacity;
        m_size = qMin(m_capacity, m_size + n);
        // The retained buffer may be clipped, but the global sample index must
        // still advance by every source sample that passed through it.
        m_totalWritten += sourceCount;
    }

    // Account for `count` samples that were lost upstream (queue drops). The
    // global index jumps over them; the retained samples are discarded because
    // getLatest() labels the buffer as one contiguous run ending at the global
    // index — keeping pre-gap samples would silently mislabel their indices
    // (and with them the TDM phase).
    void skipMany(qint64 count) {
        if (count <= 0) {
            return;
        }
        QMutexLocker locker(&m_mutex);
        m_totalWritten += count;
        m_writeIdx = 0;
        m_size = 0;
    }

    void alignNextIndex(qint64 index, bool discard = false) {
        QMutexLocker locker(&m_mutex);
        if (discard || m_totalWritten != index) {
            m_totalWritten = qMax<qint64>(0, index);
            m_writeIdx = 0;
            m_size = 0;
        }
    }

    QPair<QVector<qint64>, QVector<qint32>> getLatest(int nPoints) const {
        return getLatestInternal(nPoints, false);
    }

    QPair<QVector<qint64>, QVector<qint32>> getLatestExact(int nPoints) const {
        return getLatestInternal(nPoints, true);
    }

private:
    QPair<QVector<qint64>, QVector<qint32>> getLatestInternal(int nPoints, bool exact) const {
        QMutexLocker locker(&m_mutex);
        if (nPoints <= 0) {
            return {QVector<qint64>(), QVector<qint32>()};
        }
        if (exact && m_size < nPoints) {
            return {QVector<qint64>(), QVector<qint32>()};
        }

        const int actual = exact ? nPoints : qMin(nPoints, m_size);
        if (actual == 0) {
            return {QVector<qint64>(), QVector<qint32>()};
        }

        const int start = (m_writeIdx - actual + m_capacity) % m_capacity;
        QVector<qint32> values;
        values.reserve(actual);

        if (start + actual <= m_capacity) {
            for (int i = 0; i < actual; ++i) {
                values.push_back(m_data[start + i]);
            }
        } else {
            const int first = m_capacity - start;
            for (int i = 0; i < first; ++i) {
                values.push_back(m_data[start + i]);
            }
            for (int i = 0; i < actual - first; ++i) {
                values.push_back(m_data[i]);
            }
        }

        QVector<qint64> indices;
        indices.reserve(actual);
        const qint64 startIdx = m_totalWritten - actual;
        for (int i = 0; i < actual; ++i) {
            indices.push_back(startIdx + i);
        }
        return {indices, values};
    }

    int m_capacity;
    QVector<qint32> m_data;
    int m_writeIdx;
    int m_size;
    qint64 m_totalWritten;
    mutable QMutex m_mutex;
};

}  // namespace ccv2
