#pragma once
#include <QString>

namespace ccv2 {

constexpr int kElectrodeCount = 1024;
constexpr int kBlockCount = 64;
constexpr int kAfePerBlock = 16;
constexpr int kGlobalChannels = 256;
constexpr int kAdcPerBlock = 4;
constexpr int kStimPerBlock = 4;

struct ChannelInfo {
    int electrodeId = -1;       // 0-1023
    int blockId = -1;           // 0-63
    int idInBlock = -1;         // 0-15
    int localChannel = -1;      // 0-3 (AFE→ADC selection)
    int globalChannel = -1;     // 0-255
    int adcId = -1;             // 0-255
    int stimId = -1;            // 0-255
    int spiAddr = -1;
    int dataLane = -1;
    bool enabled = true;
    bool selected = false;
    bool saturated = false;
    bool packetLoss = false;
    bool stimActive = false;
    double rmsMicroVolt = 0.0;
    double peakToPeak = 0.0;
    double spikeRate = 0.0;
};

inline constexpr ChannelInfo kInvalidChannelInfo{};

} // namespace ccv2
