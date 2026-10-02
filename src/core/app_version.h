#pragma once

#include <QString>

namespace ccv2 {

inline constexpr const char *kAppDisplayName = "Neural Signal Command Center V6";
inline constexpr const char *kAppVersion = "6.0.4";
inline constexpr const char *kAppBuildDate = "20260929";
inline constexpr const char *kAppVersionDisplay = "6.0.4-20260929";
inline constexpr const char *kReleasePageUrl =
    "https://github.com/QianLei31/NL_CommandCenter_v5_release/releases/latest";
inline constexpr const char *kReleaseRepositoryUrl =
    "https://github.com/QianLei31/NL_CommandCenter_v5_release";

inline QString appVersionDisplay()
{
    return QString::fromLatin1(kAppVersionDisplay);
}

}  // namespace ccv2
