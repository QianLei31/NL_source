#pragma once
#include <QtGlobal>

namespace ccv2 {

// TCP response frame layout constants (replacing magic numbers from spi_control_panel.cpp:97-104)
constexpr int kSpiRespTotalBytes = 12;
constexpr int kSpiRespHeaderBytes = 8;
constexpr int kSpiRespEntryBytes = 4;

// Bit field widths within a 32-bit SPI response word
constexpr int kSpiCodeBits = 6;
constexpr int kSpiAddrBits = 10;
constexpr int kSpiDataBits = 16;

// Frame alignment for data acquisition
constexpr int kChannelsPerFrame = 256;
constexpr int kBytesPerSample = 4;  // int32
constexpr int kFrameBytes = kChannelsPerFrame * kBytesPerSample;  // 1024

struct SpiResponse {
    quint8 code = 0;
    quint16 addr = 0;
    quint32 data = 0;
};

} // namespace ccv2
