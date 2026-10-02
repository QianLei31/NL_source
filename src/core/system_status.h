#pragma once
#include <QString>

namespace ccv2 {

enum class ControlLinkState { Unknown, Checking, Reachable, Degraded, Error };
enum class AcquisitionState { Idle, Connecting, Streaming, Stalled, Error };

struct SystemStatus {
    ControlLinkState controlLink = ControlLinkState::Unknown;
    AcquisitionState acquisition = AcquisitionState::Idle;
    QString controlDetail;
    QString acquisitionDetail;
    QString host;
    int port = 7;
    int dataPort = 5001;
    double samplingRate = 20000.0;
    int activeChannels = 0;
    double packetLossRate = 0.0;
    bool recording = false;
    bool fpgaOk = false;
    bool stimEnabled = false;
    double compressionRatio = 0.0;
    bool advancedDebugUnlocked = false;
};

} // namespace ccv2
