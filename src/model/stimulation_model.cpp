#include "stimulation_model.h"
#include <cmath>

namespace ccv2 {

StimulationModel::StimulationModel(QObject *parent)
    : QObject(parent)
{
}

void StimulationModel::setParameters(const StimulationParameters &p)
{
    m_params = p;
    emit parametersChanged(p);
    emit safetyChanged(evaluateSafety(p));
}

SafetyState StimulationModel::evaluateSafety(const StimulationParameters &p)
{
    SafetyState s;
    s.violations.clear();

    // Amplitude check
    if (p.amplitudeUa < 0.0 || p.amplitudeUa > kMaxAmplitudeUa) {
        s.amplitudeOk = false;
        s.violations.append(QStringLiteral("Amplitude out of range [0, %1] µA").arg(kMaxAmplitudeUa));
    }

    // Pulse width check
    if (p.pulseWidthUs < kMinPulseWidthUs || p.pulseWidthUs > kMaxPulseWidthUs) {
        s.pulseWidthOk = false;
        s.violations.append(QStringLiteral("Pulse width out of range [%1, %2] µs")
                                .arg(kMinPulseWidthUs).arg(kMaxPulseWidthUs));
    }

    // Charge per phase check
    double chargePerPhaseNc = p.amplitudeUa * p.pulseWidthUs / 1000.0;
    if (chargePerPhaseNc > kMaxChargePerPhaseNc) {
        s.chargePerPhaseOk = false;
        s.violations.append(QStringLiteral("Charge per phase %1 nC exceeds limit %2 nC")
                                .arg(chargePerPhaseNc, 0, 'f', 1).arg(kMaxChargePerPhaseNc));
    }

    // Charge balance check (biphasic only)
    if (p.mode == StimMode::BiphasicCathodicFirst || p.mode == StimMode::BiphasicAnodicFirst) {
        // For symmetric biphasic, cathodic == anodic by definition
        // Imbalance would come from asymmetric parameters (future extension)
        // For now, symmetric biphasic is always balanced
        double chargeCathodic = chargePerPhaseNc;
        double chargeAnodic = chargePerPhaseNc;  // symmetric
        double maxCharge = std::max(chargeCathodic, chargeAnodic);
        if (maxCharge > 0) {
            double imbalance = std::abs(chargeCathodic - chargeAnodic) / maxCharge;
            if (imbalance > kChargeImbalanceThreshold) {
                s.chargeBalanceOk = false;
                s.violations.append(QStringLiteral("Charge imbalance %1% exceeds %2%")
                                        .arg(imbalance * 100.0, 0, 'f', 1)
                                        .arg(kChargeImbalanceThreshold * 100.0));
            }
        }
    }

    return s;
}

} // namespace ccv2
