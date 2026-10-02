#pragma once

#include <QObject>
#include <QString>

class QWidget;

namespace ccv2 {

struct LicenseResult {
    bool allowed{false};
    bool online{false};
    QString reason;
    QString licenseId;
    QString deviceId;
    QString product;
    QString keyHash;
    qint64 expiresAt{0};
    bool expiresAtIsNull{true};
    qint64 serverTime{0};
    QString responseSignature;
};

class LicenseClient : public QObject {
    Q_OBJECT

public:
    explicit LicenseClient(QObject *parent = nullptr);

    QString cachedLicenseKey() const;
    LicenseResult verify(const QString &licenseKey, QWidget *parentForProgress = nullptr);
    LicenseResult tryCachedOffline() const;
    void clearCache();

    static QString product();
    static QString serverBase();
    static int offlineGraceDays();

private:
    QString cachePath() const;
    QString legacyCachePath() const;
    LicenseResult tryCachedOffline(const QString &deviceId) const;
    LicenseResult tryCachedOfflineAtPath(const QString &path, const QString &deviceId) const;
    LicenseResult verifyOnline(const QString &licenseKey, const QString &deviceId, QWidget *parentForProgress);
    bool saveCache(const QString &licenseKey, const QString &deviceId, const LicenseResult &result) const;
};

}  // namespace ccv2
