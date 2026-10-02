#pragma once
#include <QObject>
#include "src/core/stim_protocol.h"

namespace ccv2 {

class StimulationModel : public QObject {
    Q_OBJECT
public:
    explicit StimulationModel(QObject *parent = nullptr);

    void setParameters(const StimulationParameters &p);
    StimulationParameters parameters() const { return m_params; }

    static SafetyState evaluateSafety(const StimulationParameters &p);

signals:
    void parametersChanged(const StimulationParameters &p);
    void safetyChanged(const SafetyState &state);

private:
    StimulationParameters m_params;
};

} // namespace ccv2
