// P1 coverage: the primitives that keep absolute frame numbering (and thus TDM
// slot parity) exact across queue drops — ThreadSafeQueue's dropped-payload
// accounting and RingBuffer::skipMany.

#include "core/threadsafe_queue.h"
#include "data/ring_buffer.h"

#include <QByteArray>

#include <iostream>

using namespace ccv2;

int main()
{
    // --- ThreadSafeQueue: pop reports bytes evicted before it, then clears ---
    {
        ThreadSafeQueue<QByteArray> q(2);
        const QByteArray a(100, 'a');   // will be dropped
        const QByteArray b(200, 'b');
        const QByteArray c(300, 'c');
        QByteArray dropped;
        q.push(a, true);
        q.push(b, true);
        // Queue full (2): pushing c drops the head (a).
        if (q.push(c, true, &dropped) || dropped != a) {
            std::cerr << "drop-oldest did not evict the head" << std::endl;
            return 1;
        }
        QByteArray out;
        qint64 before = -1;
        if (!q.pop(out, 0, &before) || out != b || before != a.size()) {
            std::cerr << "pop did not report the pre-chunk dropped bytes ("
                      << before << ")" << std::endl;
            return 2;
        }
        // The accounting must reset so the next chunk is not over-credited.
        if (!q.pop(out, 0, &before) || out != c || before != 0) {
            std::cerr << "dropped-byte accounting was not cleared on pop" << std::endl;
            return 3;
        }
    }

    // --- ThreadSafeQueue: plain pop() still clears stale drop accounting ---
    {
        ThreadSafeQueue<QByteArray> q(1);
        QByteArray dropped;
        q.push(QByteArray(64, 'x'), true);
        q.push(QByteArray(64, 'y'), true, &dropped);  // drops the 'x'
        QByteArray out;
        if (!q.pop(out, 0)) {                          // plain pop discards credit
            std::cerr << "plain pop failed" << std::endl;
            return 4;
        }
        q.push(QByteArray(64, 'z'), true);
        qint64 before = -1;
        if (!q.pop(out, 0, &before) || before != 0) {
            std::cerr << "plain pop left stale drop credit (" << before << ")" << std::endl;
            return 5;
        }
    }

    // --- RingBuffer::skipMany: index jumps, pre-gap samples are discarded ---
    {
        RingBuffer rb(8, 100);              // capacity 8, base index 100
        rb.appendMany(QVector<qint32>{1, 2, 3});   // indices 100..102
        rb.skipMany(5);                     // 5 frames lost upstream -> 103..107 gone
        rb.appendMany(QVector<qint32>{7, 8});      // indices 108..109
        const auto latest = rb.getLatest(2);
        if (latest.first != QVector<qint64>({108, 109}) ||
            latest.second != QVector<qint32>({7, 8})) {
            std::cerr << "skipMany did not advance the global index correctly"
                      << std::endl;
            return 6;
        }
        // The pre-gap samples must not resurface with wrong indices.
        const auto all = rb.getLatest(8);
        if (all.second.size() != 2) {
            std::cerr << "skipMany retained pre-gap samples" << std::endl;
            return 7;
        }
    }

    std::cout << "queue_drop_parity_smoke ok" << std::endl;
    return 0;
}
