#pragma once

#include <QPair>
#include <QString>

namespace ccv2 {

constexpr int kTdmPhaseCount = 4;

// TDM always contains four physical phases. The UI displays one of the two
// hardware pairs: local electrodes 0/2 or 1/3.
inline QPair<int, int> tdmLocalElePair(bool pair02)
{
    return pair02 ? QPair<int, int>{0, 2} : QPair<int, int>{1, 3};
}

inline int tdmPhaseForFrame(qint64 absoluteFrameIndex)
{
    const int phase = static_cast<int>(absoluteFrameIndex % kTdmPhaseCount);
    return phase < 0 ? phase + kTdmPhaseCount : phase;
}

inline int tdmVisibleSlotForPhase(int phase, bool pair02)
{
    const QPair<int, int> pair = tdmLocalElePair(pair02);
    return phase == pair.first ? 0 : (phase == pair.second ? 1 : -1);
}

// Kept for source compatibility with older callers. Routing is no longer
// block-dependent; the display pair is selected globally with tdmLocalElePair.
QPair<int, int> routedLocalElePairForChannel(int globalChannel);
int globalEleForChannelLocalEle(int globalChannel, int localEle);
QString voltageText(double volts);

} // namespace ccv2
