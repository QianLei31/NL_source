// P4 coverage: the trigger's TDM electrode selection. checkTrigger uses this
// pure mapping to evaluate the threshold on only the chosen electrode's frames;
// getting the parity wrong would let the DC step between the two interleaved
// electrodes fire the trigger on every frame.

#include "service/session_hub.h"

#include <iostream>

using namespace ccv2;

int main()
{
    // tdmSlot < 0 => non-TDM: every frame is evaluated.
    for (qint64 f = 0; f < 4; ++f) {
        if (!SessionHub::triggerFrameMatchesSlot(f, true, -1) ||
            !SessionHub::triggerFrameMatchesSlot(f, false, -1)) {
            std::cerr << "non-TDM trigger must accept every frame" << std::endl;
            return 1;
        }
    }

    // pair02=true: slot 0 = phase0, slot 1 = phase2.
    struct Case { qint64 frame; bool pair02; int slot; bool expect; };
    const Case cases[] = {
        {0, true, 0, true},  {1, true, 0, false},
        {0, true, 1, false}, {2, true, 1, true},
        {4, true, 0, true},  {6, true, 1, true},
        // pair02=false: slot 0 = phase1, slot 1 = phase3.
        {0, false, 0, false}, {1, false, 0, true},
        {3, false, 1, true},  {2, false, 1, false},
    };
    for (const Case &c : cases) {
        const bool got =
            SessionHub::triggerFrameMatchesSlot(c.frame, c.pair02, c.slot);
        if (got != c.expect) {
            std::cerr << "slot mapping wrong: frame=" << c.frame
                      << " pair02=" << c.pair02 << " slot=" << c.slot
                      << " got=" << got << " expect=" << c.expect << std::endl;
            return 2;
        }
    }

    // Within each selected pair, the two visible slots accept exactly their two
    // phases and reject the other pair.
    for (bool pair02 : {true, false}) {
        for (qint64 f = 0; f < 8; ++f) {
            const bool s0 = SessionHub::triggerFrameMatchesSlot(f, pair02, 0);
            const bool s1 = SessionHub::triggerFrameMatchesSlot(f, pair02, 1);
            const bool selectedPhase = pair02 ? ((f % 4) == 0 || (f % 4) == 2)
                                              : ((f % 4) == 1 || (f % 4) == 3);
            if ((s0 || s1) != selectedPhase || (s0 && s1)) {
                std::cerr << "electrode pair mapping wrong at frame " << f << std::endl;
                return 3;
            }
        }
    }

    std::cout << "trigger_tdm_slot_smoke ok" << std::endl;
    return 0;
}
