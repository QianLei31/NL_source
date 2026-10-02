#pragma once
#include <QObject>
#include <QTimer>
#include "src/core/stim_protocol.h"

namespace ccv2 {

class StimulationModel;

class SafetyInterlock : public QObject {
    Q_OBJECT
public:
    explicit SafetyInterlock(StimulationModel *model, QObject *parent = nullptr);

    bool engage(const StimulationParameters &p);
    void disengage();
    bool fire();
    void abort();

    InterlockState state() const { return m_state; }

signals:
    void interlockStateChanged(InterlockState state);
    void violationDetected(const SafetyState &state);
    void commandReady(const StimulationParameters &p);

private:
    void onParametersChanged();

    StimulationModel *m_model = nullptr;
    InterlockState m_state = InterlockState::Disarmed;
    StimulationParameters m_armedParams;
    bool m_paramsChangedSinceArm = false;
};

} // namespace ccv2
