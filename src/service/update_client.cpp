#include "service/update_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

#include <initializer_list>

#include "core/app_version.h"
#include "license/license_client.h"

namespace ccv2 {

namespace {

constexpr int kUpdateTimeoutMs = 5000;

QString firstNonEmpty(const QJsonObject &obj, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QString value = obj.value(QString::fromLatin1(key)).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QVector<int> versionParts(const QString &version)
{
    QVector<int> parts;
    const QRegularExpression re(QStringLiteral("(\\d+)"));
    QRegularExpressionMatchIterator it = re.globalMatch(version);
    while (it.hasNext()) {
        bool ok = false;
        const int value = it.next().captured(1).toInt(&ok);
        if (ok) {
            parts.append(value);
        }
    }
    return parts;
}

int compareParts(const QVector<int> &left, const QVector<int> &right)
{
    const int count = qMax(left.size(), right.size());
    for (int i = 0; i < count; ++i) {
        const int a = (i < left.size()) ? left.at(i) : 0;
        const int b = (i < right.size()) ? right.at(i) : 0;
        if (a < b) return -1;
        if (a > b) return 1;
    }
    return 0;
}

// The version endpoint is plain HTTP and its body is unsigned; a tampered
// download_url must never reach QDesktopServices::openUrl. Accept only
// https:// links on the project's release hosts, otherwise fall back to the
// built-in release page.
QString sanitizeDownloadUrl(const QString &candidate)
{
    const QUrl url(candidate.trimmed());
    if (!url.isValid() ||
        url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    const QString host = url.host().toLower();
    const bool trusted = host == QStringLiteral("github.com") ||
                         host.endsWith(QStringLiteral(".github.com")) ||
                         host.endsWith(QStringLiteral(".github.io"));
    return trusted ? url.toString() : QString();
}

}  // namespace

UpdateClient::UpdateClient(QObject *parent)
    : QObject(parent),
      m_manager(new QNetworkAccessManager(this)),
      m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
}

UpdateClient::~UpdateClient()
{
    m_timeout->stop();
    if (m_reply) {
        // abort() emits finished synchronously. Detach the pending reply before
        // aborting so its handler cannot clear m_reply or report a stale result.
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

QString UpdateClient::updateEndpoint()
{
    return LicenseClient::serverBase() + QStringLiteral("/v1/app-version");
}

QString UpdateClient::defaultDownloadUrl()
{
    return QString::fromLatin1(kReleasePageUrl);
}

int UpdateClient::compareVersions(const QString &lhs, const QString &rhs)
{
    // Published tags use numeric stable versions or -preview.N, and the UI
    // optionally appends -YYYYMMDD. A final 7.0.0 must supersede its preview
    // even though the preview's numeric suffix/date would otherwise rank higher.
    // Keep legacy numeric ordering for same-kind and unrecognized versions.
    static const QRegularExpression releaseForm(
        QStringLiteral("^v?(\\d+(?:\\.\\d+)*)(-preview(?:\\.\\d+)+)?(?:-\\d{8})?$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto leftRelease = releaseForm.match(lhs.trimmed());
    const auto rightRelease = releaseForm.match(rhs.trimmed());
    if (leftRelease.hasMatch() && rightRelease.hasMatch()) {
        const bool leftPreview = !leftRelease.captured(2).isEmpty();
        const bool rightPreview = !rightRelease.captured(2).isEmpty();
        if (leftPreview || rightPreview) {
            const int coreOrder = compareParts(versionParts(leftRelease.captured(1)),
                                               versionParts(rightRelease.captured(1)));
            if (coreOrder != 0) return coreOrder;
            if (leftPreview != rightPreview) return leftPreview ? -1 : 1;
        }
    }
    // Equal numeric fields mean the same version: v6.0.1 == 6.0.1.0.
    return compareParts(versionParts(lhs), versionParts(rhs));
}

void UpdateClient::checkForUpdates(const QString &currentVersion)
{
    m_timeout->stop();
    if (m_reply) {
        // abort() emits finished synchronously. Detach the pending reply before
        // aborting so its handler cannot clear m_reply or report a stale result.
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }

    QUrl url(updateEndpoint());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("product"), LicenseClient::product());
    query.addQueryItem(QStringLiteral("current_version"), currentVersion);
    query.addQueryItem(QStringLiteral("channel"), QStringLiteral("stable"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("NL_CommandCenter/%1").arg(currentVersion));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
#endif

    emit checkStarted();

    m_reply = m_manager->get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this, currentVersion]() {
        handleReply(currentVersion);
    });
    disconnect(m_timeout, nullptr, this, nullptr);
    connect(m_timeout, &QTimer::timeout, this, [this, currentVersion]() {
        if (m_reply) {
            // abort() synchronously emits finished -> handleReply would emit a
            // second checkFinished; detach it first so this path reports once.
            disconnect(m_reply, &QNetworkReply::finished, this, nullptr);
            m_reply->abort();
        }
        finishWithError(currentVersion, QStringLiteral("更新服务器连接超时"));
    });
    m_timeout->start(kUpdateTimeoutMs);
}

void UpdateClient::finishWithError(const QString &currentVersion, const QString &error)
{
    if (m_timeout->isActive()) {
        m_timeout->stop();
    }
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    UpdateCheckResult result;
    result.online = false;
    result.currentVersion = currentVersion;
    result.downloadUrl = defaultDownloadUrl();
    result.error = error;
    emit checkFinished(result);
}

void UpdateClient::handleReply(const QString &currentVersion)
{
    if (!m_reply) {
        return;
    }

    if (m_timeout->isActive()) {
        m_timeout->stop();
    }

    QNetworkReply *reply = m_reply;
    m_reply = nullptr;

    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString errorText = reply->errorString();
    reply->deleteLater();

    if (status == 404) {
        UpdateCheckResult result;
        result.online = true;
        result.currentVersion = currentVersion;
        result.latestVersion = currentVersion;
        result.downloadUrl = defaultDownloadUrl();
        result.updateAvailable = false;
        result.message = QStringLiteral("更新服务器已连接，但暂未发布版本信息；可打开 GitHub 下载页查看最新 release。");
        emit checkFinished(result);
        return;
    }

    if (netError != QNetworkReply::NoError || (status > 0 && (status < 200 || status >= 300))) {
        UpdateCheckResult result;
        result.online = false;
        result.currentVersion = currentVersion;
        result.downloadUrl = defaultDownloadUrl();
        result.error = status > 0
                           ? QStringLiteral("更新服务器返回 HTTP %1").arg(status)
                           : errorText;
        emit checkFinished(result);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        finishWithError(currentVersion, QStringLiteral("更新服务器返回格式错误"));
        return;
    }

    const QJsonObject obj = doc.object();
    const QString latest = firstNonEmpty(obj, {"latest_version", "version", "tag", "tag_name"});
    if (latest.isEmpty()) {
        finishWithError(currentVersion, QStringLiteral("更新服务器缺少 latest_version 字段"));
        return;
    }

    UpdateCheckResult result;
    result.online = true;
    result.currentVersion = currentVersion;
    result.latestVersion = latest;
    result.downloadUrl =
        sanitizeDownloadUrl(firstNonEmpty(obj, {"download_url", "release_url", "github_url", "url"}));
    if (result.downloadUrl.isEmpty()) {
        result.downloadUrl = defaultDownloadUrl();
    }
    result.message = firstNonEmpty(obj, {"message", "notes", "description"});
    result.updateAvailable = obj.contains(QStringLiteral("update_available"))
                                 ? obj.value(QStringLiteral("update_available")).toBool(false)
                                 : compareVersions(latest, currentVersion) > 0;
    emit checkFinished(result);
}

}  // namespace ccv2
