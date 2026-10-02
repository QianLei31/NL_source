#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace ccv2 {

struct UpdateCheckResult {
    bool online{false};
    bool updateAvailable{false};
    QString currentVersion;
    QString latestVersion;
    QString downloadUrl;
    QString message;
    QString error;
};

class UpdateClient : public QObject {
    Q_OBJECT

public:
    explicit UpdateClient(QObject *parent = nullptr);
    ~UpdateClient() override;

    void checkForUpdates(const QString &currentVersion);

    static QString updateEndpoint();
    static QString defaultDownloadUrl();
    static int compareVersions(const QString &lhs, const QString &rhs);

signals:
    void checkStarted();
    void checkFinished(const ccv2::UpdateCheckResult &result);

private:
    void finishWithError(const QString &currentVersion, const QString &error);
    void handleReply(const QString &currentVersion);

    QNetworkAccessManager *m_manager{nullptr};
    QNetworkReply *m_reply{nullptr};
    QTimer *m_timeout{nullptr};
};

}  // namespace ccv2
