#pragma once

namespace ccv2 {

constexpr int kChannelsTotal = 256;
constexpr int kBytesPerPoint = 4;
constexpr int kFrameBytes = kChannelsTotal * kBytesPerPoint;
constexpr int kTcpBuf = 4096;
constexpr int kAdcBits = 12;
constexpr double kVref = 1.8;
// Each 32-bit point is {timestamp[31:12], adc[11:0]} (ctrl_80m, 80M duplex FW).
// The ADC sample is the low 12 bits; the high 20 bits are the frame timestamp.
constexpr unsigned kAdcSampleMask = 0x0FFFu;
constexpr int kTimestampShift = 12;

}  // namespace ccv2
