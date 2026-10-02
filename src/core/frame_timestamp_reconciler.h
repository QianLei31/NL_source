#pragma once

#include <QtGlobal>

namespace ccv2 {

// Reconciles the software frame index with the hardware per-frame timestamp
// (the high 20 bits of each point, which increment by 1 per frame, mod 2^20,
// and are identical across a frame's 256 channel words).
//
// Why: the TDM electrode a frame belongs to is a property of the true hardware
// frame number modulo four. The software counter only sees frames that reached
// it, so an upstream loss can rotate every subsequent phase. Feeding the
// timestamp here preserves the modulo-four index across losses.
//
// The reconciler stays dormant (pure pass-through of the software count) until
// it observes a reliable +1 counter, so streams with an absent/constant/zero
// timestamp (older firmware) behave exactly as before. A constant value or an
// implausibly large jump disarms it (counter reset / reconnect) until the next
// clean +1 step.
//
// It does NOT try to align the index to the timestamp's absolute value: only
// gap-consistency matters. A gapless
// stream therefore returns exactly the software count — identical to the
// previous behaviour — so only real upstream drops change anything.
class FrameTimestampReconciler {
public:
    // Restart at a software base index (e.g. on a seek / new acquisition).
    void reset(qint64 baseIndex) {
        m_index = baseIndex;
        m_prevTs = 0;
        m_havePrev = false;
        m_active = false;
        m_totalGap = 0;
        m_knownSkipped = 0;
    }

    void skipFrames(qint64 count) {
        if (count <= 0) return;
        m_index += count;
        m_knownSkipped += count;
    }

    // Consume one frame's timestamp and return that frame's corrected absolute
    // index (software count, advanced over any inferred upstream loss so parity
    // stays consistent across the gap). `gapBefore`, if provided, receives the
    // number of frames inferred lost immediately before this one.
    qint64 advance(quint32 timestamp, qint64 *gapBefore = nullptr) {
        constexpr quint32 kMask = 0xFFFFFu;      // 20-bit counter
        constexpr quint32 kHalf = 1u << 19;      // half-range -> treat as reset
        const quint32 ts = timestamp & kMask;
        qint64 gap = 0;

        if (m_havePrev) {
            const quint32 delta = (ts - m_prevTs) & kMask;
            if (!m_active) {
                if (static_cast<qint64>(delta) == m_knownSkipped + 1) {
                    m_active = true;   // reliable +1 counter confirmed
                }
            } else if (delta == 0 || delta >= kHalf) {
                m_active = false;   // constant / huge jump: counter unreliable
            } else if (static_cast<qint64>(delta) > m_knownSkipped + 1) {
                gap = static_cast<qint64>(delta) - 1 - m_knownSkipped;
                m_index += gap;
                m_totalGap += gap;
            }
        }

        m_prevTs = ts;
        m_knownSkipped = 0;
        m_havePrev = true;
        if (gapBefore) {
            *gapBefore = gap;
        }
        const qint64 idx = m_index;
        ++m_index;
        return idx;
    }

    // True once a reliable +1 timestamp counter has been observed; while false,
    // callers should fall back to their software frame count.
    bool active() const { return m_active; }
    // Cumulative frames inferred lost upstream since the last reset.
    qint64 totalGap() const { return m_totalGap; }
    // Next absolute index that advance() would return.
    qint64 nextIndex() const { return m_index; }

private:
    qint64 m_index{0};
    quint32 m_prevTs{0};
    bool m_havePrev{false};
    bool m_active{false};
    qint64 m_totalGap{0};
    qint64 m_knownSkipped{0};
};

}  // namespace ccv2
