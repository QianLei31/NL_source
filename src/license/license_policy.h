#pragma once

#include <QDate>
#include <QString>

namespace ccv2 {

// This V6 release is license-free through 2026-12-21. Starting on
// 2026-12-22, startup falls back to the existing online/offline license flow.
inline const QDate &licenseRequiredFromDate()
{
    static const QDate date(2026, 12, 22);
    return date;
}

inline bool isLicenseGracePeriod(const QDate &today = QDate::currentDate())
{
    return today.isValid() && today < licenseRequiredFromDate();
}

inline int licenseGraceDaysRemaining(const QDate &today = QDate::currentDate())
{
    if (!isLicenseGracePeriod(today)) {
        return 0;
    }
    return today.daysTo(licenseRequiredFromDate());
}

inline QString licenseRequiredFromDisplay()
{
    return licenseRequiredFromDate().toString(QStringLiteral("yyyy-MM-dd"));
}

}  // namespace ccv2
