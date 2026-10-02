#include <QApplication>
#include <QCoreApplication>
#include <QNetworkProxy>

#include "core/app_version.h"
#include "license/license_client.h"
#include "license/license_dialog.h"
#include "license/license_policy.h"
#include "service/dummy_stream_server.h"
#include "ui/command_center_main_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("NeuralLab"));
    QCoreApplication::setApplicationName(QStringLiteral("NL_CommandCenter_v6_qt"));
    QCoreApplication::setApplicationVersion(ccv2::appVersionDisplay());
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    if (QCoreApplication::arguments().contains(QStringLiteral("--dummy-server"))) {
        return ccv2::runDummyServerMode(app, QCoreApplication::arguments());
    }

    if (!ccv2::isLicenseGracePeriod()) {
        ccv2::LicenseClient licenseClient;
        bool licenseAccepted = false;
        const QString cachedKey = licenseClient.cachedLicenseKey().trimmed();
        if (!cachedKey.isEmpty()) {
            const ccv2::LicenseResult cached = licenseClient.tryCachedOffline();
            if (cached.allowed) {
                licenseAccepted = true;
            } else {
                const ccv2::LicenseResult refreshed = licenseClient.verify(cachedKey);
                licenseAccepted = refreshed.allowed;
            }
        }

        if (!licenseAccepted) {
            ccv2::LicenseDialog licenseDialog;
            if (licenseDialog.exec() != QDialog::Accepted) {
                return 0;
            }
        }
    }

    ccv2::CommandCenterMainWindow window;
    window.show();
    return app.exec();
}
