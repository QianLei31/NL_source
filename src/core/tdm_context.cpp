#include "core/tdm_context.h"

namespace ccv2 {

TdmContext::TdmContext(QObject *parent) : QObject(parent) {}

void TdmContext::setEnabled(bool enabled) {
    setState(enabled, pair02());
}

void TdmContext::setPair02(bool pair02) {
    setState(enabled(), pair02);
}

void TdmContext::setEvenFirst(bool evenFirst) {
    setPair02(evenFirst);
}

void TdmContext::setState(bool enabled, bool pair02) {
    const bool wasEnabled = m_enabled.exchange(enabled, std::memory_order_relaxed);
    const bool wasPair02 = m_pair02.exchange(pair02, std::memory_order_relaxed);
    if (wasEnabled != enabled || wasPair02 != pair02) {
        emit changed(enabled, pair02);
    }
}

}  // namespace ccv2
