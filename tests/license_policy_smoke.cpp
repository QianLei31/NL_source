#include "license/license_policy.h"

#include <QCoreApplication>

#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (!ccv2::isLicenseGracePeriod(QDate(2026, 6, 21))) {
        std::cerr << "release date must be inside grace period" << std::endl;
        return 1;
    }
    if (!ccv2::isLicenseGracePeriod(QDate(2026, 12, 21))) {
        std::cerr << "last grace day must not require a license" << std::endl;
        return 2;
    }
    if (ccv2::isLicenseGracePeriod(QDate(2026, 12, 22))) {
        std::cerr << "license must be required from 2026-12-22" << std::endl;
        return 3;
    }
    if (ccv2::licenseGraceDaysRemaining(QDate(2026, 12, 21)) != 1) {
        std::cerr << "remaining-day calculation failed" << std::endl;
        return 4;
    }

    std::cout << "license_policy_smoke ok required_from="
              << ccv2::licenseRequiredFromDisplay().toStdString() << std::endl;
    return 0;
}
