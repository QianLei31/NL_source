#include "safety_interlock.h"
#include "src/model/stimulation_model.h"

namespace ccv2 {

SafetyInterlock::SafetyInterlock(StimulationModel *model, QObject *parent)
    : QObject(parent)
    , m_model(model)
{
    connect(m_model, &StimulationModel::parametersChanged, this, &SafetyInterlock::onParametersChanged);
}

bool SafetyInterlock::engage(const StimulationParameters &p)
{
    SafetyState safety = StimulationModel::evaluateSafety(p);
    if (!safety.ok()) {
        emit violationDetected(safety);
        return false;
    }

    m_armedParams = p;
    m_paramsChangedSinceArm = false;
    m_state = InterlockState::Armed;
    emit interlockStateChanged(m_state);
    return true;
}

void SafetyInterlock::disengage()
{
    m_state = InterlockState::Disarmed;
    emit interlockStateChanged(m_state);
}

bool SafetyInterlock::fire()
{
    if (m_state != InterlockState::Armed) return false;
    if (m_paramsChangedSinceArm) {
        // Parameters changed since arm — must re-engage
        m_state = InterlockState::Disarmed;
        emit interlockStateChanged(m_state);
        return false;
    }

    m_state = InterlockState::Fired;
    emit interlockStateChanged(m_state);
    emit commandReady(m_armedParams);

    // Auto-return to disarmed after fire
    m_state = InterlockState::Disarmed;
    emit interlockStateChanged(m_state);
    return true;
}

void SafetyInterlock::abort()
{
    m_state = InterlockState::Disarmed;
    emit interlockStateChanged(m_state);
}

void SafetyInterlock::onParametersChanged()
{
    if (m_state == InterlockState::Armed) {
        m_paramsChangedSinceArm = true;
    }
}

} // namespace ccv2
