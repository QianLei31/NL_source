// Unit coverage for FrameTimestampReconciler: the component that turns the
// hardware per-frame timestamp into a drop-robust absolute index whose parity
// is the true TDM electrode slot.

#include "core/frame_timestamp_reconciler.h"

#include <iostream>
#include <vector>

using namespace ccv2;

int main()
{
    // 1) Dormant on a zero / non-incrementing stream: pure pass-through count,
    //    never armed (older firmware must behave exactly as before).
    {
        FrameTimestampReconciler r;
        r.reset(0);
        for (int i = 0; i < 10; ++i) {
            qint64 gap = -1;
            const qint64 idx = r.advance(0, &gap);
            if (idx != i || gap != 0 || r.active()) {
                std::cerr << "zero stream should stay dormant (i=" << i << ")" << std::endl;
                return 1;
            }
        }
    }

    // 2) A gapless stream returns exactly the software count (no behaviour
    //    change vs. before), regardless of the timestamp's absolute value.
    {
        FrameTimestampReconciler r;
        r.reset(100);
        const std::vector<quint32> ts = {5000, 5001, 5002, 5003, 5004};
        for (size_t i = 0; i < ts.size(); ++i) {
            qint64 gap = -1;
            const qint64 idx = r.advance(ts[i], &gap);
            if (idx != 100 + static_cast<qint64>(i) || gap != 0) {
                std::cerr << "gapless stream must return the plain count" << std::endl;
                return 2;
            }
        }
        if (!r.active()) {
            std::cerr << "should be armed after consecutive +1" << std::endl;
            return 3;
        }
    }

    // 3) The core fix: an ODD upstream gap advances the index over the lost
    //    frame, so parity stays consistent (the surviving frame keeps its true
    //    offset). Frame 13 is lost: 10,11,12,_,14,15 from base 0.
    {
        FrameTimestampReconciler r;
        r.reset(0);
        qint64 gap = 0;
        r.advance(10);                            // idx 0
        r.advance(11);                            // idx 1 (armed)
        const qint64 i12 = r.advance(12);         // idx 2
        const qint64 i14 = r.advance(14, &gap);   // delta 2 -> 1 lost, idx 4
        const qint64 i15 = r.advance(15);         // idx 5
        if (gap != 1 || r.totalGap() != 1) {
            std::cerr << "gap of 1 frame not detected (gap=" << gap << ")" << std::endl;
            return 4;
        }
        // 14 is 4 frames after the base frame 10, so its index must be 4 — the
        // lost frame 13 (index 3) is skipped, not collapsed. A naive counter
        // would have given 14 index 3, flipping the electrode from here on.
        if (i12 != 2 || i14 != 4 || i15 != 5) {
            std::cerr << "index did not advance over the lost frame ("
                      << i12 << "," << i14 << "," << i15 << ")" << std::endl;
            return 5;
        }
    }

    // 5) A constant value or an implausible jump disarms; a clean +1 re-arms.
    {
        FrameTimestampReconciler r;
        r.reset(0);
        r.advance(10);
        r.advance(11);            // armed
        if (!r.active()) { std::cerr << "expected armed" << std::endl; return 8; }
        r.advance(11);            // delta 0 -> disarm
        if (r.active()) { std::cerr << "constant should disarm" << std::endl; return 9; }
        r.advance(600000);        // huge jump while disarmed: stays disarmed
        r.advance(600001);        // clean +1 -> re-arm
        if (!r.active()) { std::cerr << "should re-arm on +1" << std::endl; return 10; }
    }

    std::cout << "frame_timestamp_reconciler_smoke ok" << std::endl;
    return 0;
}
