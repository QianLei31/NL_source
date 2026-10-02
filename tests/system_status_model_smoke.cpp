#include "model/system_status_model.h"

#include <QCoreApplication>

#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ccv2::SystemStatusModel model;

    model.setControlLink(ccv2::ControlLinkState::Reachable,
                         QStringLiteral("SPI write succeeded"));
    model.setAcquisition(ccv2::AcquisitionState::Streaming,
                         QStringLiteral("DMA active"));

    // A user stop is an acquisition transition, not a device disconnect.
    model.setAcquisition(ccv2::AcquisitionState::Idle,
                         QStringLiteral("user stop"));
    ccv2::SystemStatus status = model.snapshot();
    if (status.controlLink != ccv2::ControlLinkState::Reachable ||
        status.acquisition != ccv2::AcquisitionState::Idle) {
        std::cerr << "stopping acquisition cleared control health" << std::endl;
        return 1;
    }

    // A data-only fault must also preserve the independently proven control path.
    model.setAcquisition(ccv2::AcquisitionState::Stalled,
                         QStringLiteral("no DMA data"));
    status = model.snapshot();
    if (status.controlLink != ccv2::ControlLinkState::Reachable ||
        status.acquisition != ccv2::AcquisitionState::Stalled) {
        std::cerr << "data fault contaminated control health" << std::endl;
        return 2;
    }

    model.setControlLink(ccv2::ControlLinkState::Degraded,
                         QStringLiteral("write succeeded; reply timeout"));
    status = model.snapshot();
    if (status.controlLink != ccv2::ControlLinkState::Degraded ||
        status.acquisition != ccv2::AcquisitionState::Stalled) {
        std::cerr << "control update contaminated acquisition state" << std::endl;
        return 3;
    }

    std::cout << "system_status_model_smoke ok" << std::endl;
    return 0;
}
