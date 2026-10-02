#pragma once
#include <QString>
#include <QStringList>

namespace ccv2 {

constexpr double kMaxAmplitudeUa = 2000.0;
constexpr double kMinPulseWidthUs = 50.0;
constexpr double kMaxPulseWidthUs = 500.0;
constexpr double kMaxChargePerPhaseNc = 30.0;
constexpr double kChargeImbalanceThreshold = 0.05;

enum class StimMode { Monophasic, BiphasicCathodicFirst, BiphasicAnodicFirst };
enum class StimTrigger { Manual, FpgaSpike, ExternalTTL, Software };
enum class InterlockState { Disarmed, Armed, Fired };

struct StimulationParameters {
    int channelId = 0;
    StimMode mode = StimMode::BiphasicCathodicFirst;
    double amplitudeUa = 0.0;
    double pulseWidthUs = 200.0;
    double interphaseGapUs = 20.0;
    double frequencyHz = 100.0;
    double trainDurationS = 1.0;
    StimTrigger trigger = StimTrigger::Manual;
};

struct SafetyState {
    bool amplitudeOk = true;
    bool pulseWidthOk = true;
    bool chargePerPhaseOk = true;
    bool chargeBalanceOk = true;
    QStringList violations;

    bool ok() const { return violations.isEmpty(); }
};

} // namespace ccv2
