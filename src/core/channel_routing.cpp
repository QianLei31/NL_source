#include "core/channel_routing.h"

#include <cmath>

namespace ccv2 {

namespace {

constexpr int kRoutingLocalChannelsPerBlock = 4;
constexpr int kRoutingLocalElesPerChannel = 4;

} // namespace

QPair<int, int> routedLocalElePairForChannel(int globalChannel)
{
    Q_UNUSED(globalChannel);
    return tdmLocalElePair(true);
}

int globalEleForChannelLocalEle(int globalChannel, int localEle)
{
    const int block = globalChannel / kRoutingLocalChannelsPerBlock;
    const int localChannel = globalChannel % kRoutingLocalChannelsPerBlock;
    return block * 16 + localChannel * kRoutingLocalElesPerChannel + localEle;
}

QString voltageText(double volts)
{
    if (!std::isfinite(volts)) {
        return QStringLiteral("-");
    }
    if (std::abs(volts) >= 1.0) {
        return QStringLiteral("%1 V").arg(volts, 0, 'f', 3);
    }
    return QStringLiteral("%1 mV").arg(volts * 1000.0, 0, 'f', 2);
}

} // namespace ccv2
