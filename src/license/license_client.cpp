#include "license/license_client.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <array>

#include <openssl/evp.h>

#include "license/device_fingerprint.h"

namespace ccv2 {

namespace {

constexpr int kNetworkTimeoutMs = 8000;
constexpr int kMaxServerClockSkewSeconds = 10 * 60;
constexpr int kCacheFormatVersion = 2;

constexpr std::array<unsigned char, 32> kServerEd25519PublicKey = {
    0x92, 0xaf, 0xf9, 0x1f, 0xb8, 0x54, 0x4c, 0xee,
    0x6d, 0x5a, 0x68, 0xd1, 0x47, 0x40, 0x15, 0x17,
    0xff, 0x58, 0x9e, 0xf1, 0x3c, 0x72, 0x47, 0x76,
    0x46, 0x82, 0xce, 0x98, 0x28, 0x6e, 0x75, 0x0c,
};

const QByteArray kCacheHmacKey =
    QByteArrayLiteral("NLCCv5 license cache hmac key 2026-06-17 / do not edit cache by hand");
const QByteArray kLegacyCacheHmacKey =
    QByteArrayLiteral("NLCCv4 license cache hmac key 2026-06-17 / do not edit cache by hand");

QJsonObject readJsonFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

QString cacheDirForAppName(const QString &windowsName, const QString &homeName) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString dir = base.isEmpty()
                      ? QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                            .filePath(homeName)
                      : QDir(base).filePath(windowsName);
    QDir().mkpath(dir);
    return dir;
}

QString currentCacheDir() {
    return cacheDirForAppName(QStringLiteral("NL_CommandCenter_v5_qt"),
                              QStringLiteral(".nl_command_center_v5"));
}

QString legacyCacheDir() {
    return cacheDirForAppName(QStringLiteral("NL_CommandCenter_v4_qt"),
                              QStringLiteral(".nl_command_center_v4"));
}

bool isSupportedLicenseProduct(const QString &product) {
    return product == QStringLiteral("nl_command_center_v4") ||
           product == QStringLiteral("nl_command_center_v5") ||
           product == QStringLiteral("nl_command_center_v6") ||
           product == QStringLiteral("nl_command_center");
}

QString reasonFromHttp(int status, const QByteArray &body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        const QString detail = obj.value(QStringLiteral("detail")).toString();
        if (!detail.isEmpty()) {
            return detail;
        }
    }
    return QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(body).left(300));
}

QByteArray base64UrlDecode(const QString &text) {
    QByteArray data = text.toLatin1();
    data.replace('-', '+');
    data.replace('_', '/');
    while (data.size() % 4 != 0) {
        data.append('=');
    }
    return QByteArray::fromBase64(data);
}

QString sha256Hex(const QString &text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString jsonStringLiteral(const QString &value) {
    QString out = QStringLiteral("\"");
    out.reserve(value.size() + 2);
    for (const QChar ch : value) {
        const ushort u = ch.unicode();
        switch (u) {
        case '\"':
            out += QStringLiteral("\\\"");
            break;
        case '\\':
            out += QStringLiteral("\\\\");
            break;
        case '\b':
            out += QStringLiteral("\\b");
            break;
        case '\f':
            out += QStringLiteral("\\f");
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        case '\t':
            out += QStringLiteral("\\t");
            break;
        default:
            if (u < 0x20 || u > 0x7e) {
                out += QStringLiteral("\\u") + QString::number(u, 16).rightJustified(4, QLatin1Char('0'));
            } else {
                out += ch;
            }
            break;
        }
    }
    out += QStringLiteral("\"");
    return out;
}

QByteArray signedPayloadBytes(const LicenseResult &result) {
    if (result.allowed) {
        const QString expiresAt = result.expiresAtIsNull ? QStringLiteral("null") : QString::number(result.expiresAt);
        return QStringLiteral("{\"device_id\":%1,\"expires_at\":%2,\"key_hash\":%3,\"license_id\":%4,\"product\":%5,\"server_time\":%6,\"valid\":true}")
            .arg(jsonStringLiteral(result.deviceId),
                 expiresAt,
                 jsonStringLiteral(result.keyHash),
                 jsonStringLiteral(result.licenseId),
                 jsonStringLiteral(result.product),
                 QString::number(result.serverTime))
            .toUtf8();
    }

    return QStringLiteral("{\"device_id\":%1,\"key_hash\":%2,\"product\":%3,\"reason\":%4,\"server_time\":%5,\"valid\":false}")
        .arg(jsonStringLiteral(result.deviceId),
             jsonStringLiteral(result.keyHash),
             jsonStringLiteral(result.product),
             jsonStringLiteral(result.reason),
             QString::number(result.serverTime))
        .toUtf8();
}

bool verifyEd25519(const QByteArray &message, const QString &signatureText) {
    const QByteArray signature = base64UrlDecode(signatureText);
    if (signature.size() != 64) {
        return false;
    }

    EVP_PKEY *rawKey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,
                                                   nullptr,
                                                   kServerEd25519PublicKey.data(),
                                                   kServerEd25519PublicKey.size());
    if (!rawKey) {
        return false;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(rawKey);
        return false;
    }

    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, rawKey) == 1) {
        ok = EVP_DigestVerify(ctx,
                              reinterpret_cast<const unsigned char *>(signature.constData()),
                              static_cast<size_t>(signature.size()),
                              reinterpret_cast<const unsigned char *>(message.constData()),
                              static_cast<size_t>(message.size())) == 1;
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(rawKey);
    return ok;
}

bool verifyResponseSignature(const LicenseResult &result) {
    if (result.responseSignature.isEmpty() || result.serverTime <= 0) {
        return false;
    }

    return verifyEd25519(signedPayloadBytes(result), result.responseSignature);
}

bool verifyFreshResponseSignature(const LicenseResult &result) {
    if (!verifyResponseSignature(result)) {
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (qAbs(now - result.serverTime) > kMaxServerClockSkewSeconds) {
        return false;
    }

    return true;
}

QJsonObject cachePayload(const QJsonObject &obj) {
    QJsonObject payload = obj;
    payload.remove(QStringLiteral("cache_hmac"));
    return payload;
}

QByteArray canonicalCacheBytes(const QJsonObject &obj) {
    return QJsonDocument(cachePayload(obj)).toJson(QJsonDocument::Compact);
}

QString cacheHmacWithKey(const QJsonObject &obj, const QByteArray &key) {
    const QByteArray mac = QMessageAuthenticationCode::hash(canonicalCacheBytes(obj),
                                                            key,
                                                            QCryptographicHash::Sha256);
    return QString::fromLatin1(mac.toHex());
}

QString cacheHmac(const QJsonObject &obj) {
    return cacheHmacWithKey(obj, kCacheHmacKey);
}

bool constantTimeEqual(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(static_cast<unsigned char>(a.at(i)) ^
                                           static_cast<unsigned char>(b.at(i)));
    }
    return diff == 0;
}

bool hasValidCacheHmac(const QJsonObject &obj) {
    const QString stored = obj.value(QStringLiteral("cache_hmac")).toString();
    if (stored.isEmpty()) {
        return false;
    }
    const QByteArray storedBytes = stored.toUtf8();
    return constantTimeEqual(storedBytes, cacheHmacWithKey(obj, kCacheHmacKey).toUtf8()) ||
           constantTimeEqual(storedBytes, cacheHmacWithKey(obj, kLegacyCacheHmacKey).toUtf8());
}

}  // namespace

LicenseClient::LicenseClient(QObject *parent) : QObject(parent) {}

QString LicenseClient::product() {
    // License product is intentionally stable across app releases.
    // The UI can be V5/V6, while existing V4-issued keys keep working.
    return QStringLiteral("nl_command_center_v4");
}

QString LicenseClient::serverBase() {
    return QStringLiteral("http://47.101.131.245");
}

int LicenseClient::offlineGraceDays() {
    return 90;
}

QString LicenseClient::cachePath() const {
    return QDir(currentCacheDir()).filePath(QStringLiteral("license_cache.json"));
}

QString LicenseClient::legacyCachePath() const {
    return QDir(legacyCacheDir()).filePath(QStringLiteral("license_cache.json"));
}

QString LicenseClient::cachedLicenseKey() const {
    const QJsonObject obj = readJsonFile(cachePath());
    const QString currentKey = obj.value(QStringLiteral("license_key")).toString();
    if (!currentKey.trimmed().isEmpty()) {
        return currentKey;
    }
    return readJsonFile(legacyCachePath()).value(QStringLiteral("license_key")).toString();
}

LicenseResult LicenseClient::verify(const QString &licenseKey, QWidget *parentForProgress) {
    const QString normalizedKey = licenseKey.trimmed();
    if (normalizedKey.isEmpty()) {
        return {false, false, QStringLiteral("请输入 license key")};
    }

    const QString deviceId = deviceFingerprint();
    LicenseResult onlineResult = verifyOnline(normalizedKey, deviceId, parentForProgress);
    if (onlineResult.allowed) {
        saveCache(normalizedKey, deviceId, onlineResult);
        return onlineResult;
    }

    if (!onlineResult.online) {
        LicenseResult offline = tryCachedOffline(deviceId);
        if (offline.allowed && cachedLicenseKey().trimmed() == normalizedKey) {
            offline.reason = QStringLiteral("授权服务器暂时不可用，已使用离线宽限");
            return offline;
        }
    }

    return onlineResult;
}

LicenseResult LicenseClient::verifyOnline(const QString &licenseKey, const QString &deviceId, QWidget *parentForProgress) {
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(serverBase() + QStringLiteral("/v1/verify")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonObject body;
    body.insert(QStringLiteral("key"), licenseKey);
    body.insert(QStringLiteral("product"), product());
    body.insert(QStringLiteral("device_id"), deviceId);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QNetworkReply *reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QProgressDialog *progress = nullptr;
    if (parentForProgress) {
        progress = new QProgressDialog(QStringLiteral("正在校验授权..."), QString(), 0, 0, parentForProgress);
        progress->setWindowTitle(QStringLiteral("License"));
        progress->setCancelButton(nullptr);
        progress->setWindowModality(Qt::ApplicationModal);
        progress->setMinimumDuration(300);
        progress->show();
    }

    timer.start(kNetworkTimeoutMs);
    loop.exec();
    if (progress) {
        progress->close();
        progress->deleteLater();
    }

    LicenseResult result;
    result.deviceId = deviceId;
    result.product = product();
    result.keyHash = sha256Hex(licenseKey);

    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
        reply->deleteLater();
        result.allowed = false;
        result.online = false;
        result.reason = QStringLiteral("连接授权服务器超时");
        return result;
    }

    const QByteArray responseBytes = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status > 0 && (status < 200 || status >= 300)) {
        // 5xx, proxy errors and captive-portal redirects are not an
        // authoritative license decision. online=false lets verify() fall
        // back to the offline grace cache instead of locking the user out.
        result.allowed = false;
        result.online = false;
        result.reason = reasonFromHttp(status, responseBytes);
        reply->deleteLater();
        return result;
    }

    if (reply->error() != QNetworkReply::NoError) {
        result.allowed = false;
        result.online = false;
        result.reason = reply->errorString();
        reply->deleteLater();
        return result;
    }

    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes);
    if (!doc.isObject()) {
        // Typical captive portal: HTTP 200 with an HTML login page.
        result.allowed = false;
        result.online = false;
        result.reason = QStringLiteral("授权服务器返回格式错误");
        return result;
    }

    const QJsonObject obj = doc.object();
    result.allowed = obj.value(QStringLiteral("valid")).toBool(false);
    result.reason = obj.value(QStringLiteral("reason")).toString();
    result.licenseId = obj.value(QStringLiteral("license_id")).toString();
    result.product = obj.value(QStringLiteral("product")).toString();
    result.deviceId = obj.value(QStringLiteral("device_id")).toString();
    result.keyHash = obj.value(QStringLiteral("key_hash")).toString();
    result.expiresAtIsNull = obj.value(QStringLiteral("expires_at")).isNull() ||
                             obj.value(QStringLiteral("expires_at")).isUndefined();
    result.expiresAt = result.expiresAtIsNull ? 0 : obj.value(QStringLiteral("expires_at")).toVariant().toLongLong();
    result.serverTime = obj.value(QStringLiteral("server_time")).toVariant().toLongLong();
    result.responseSignature = obj.value(QStringLiteral("sig")).toString();

    if (!result.allowed && result.reason.isEmpty()) {
        result.reason = QStringLiteral("授权无效");
    }

    // Only a response that matches this request AND carries a valid fresh
    // Ed25519 signature counts as an authoritative online decision (allow or
    // deny). Anything else — tampered, mismatched, unsigned — is treated as
    // "server unavailable" so the offline grace cache still applies.
    if (!isSupportedLicenseProduct(result.product) || result.deviceId != deviceId || result.keyHash != sha256Hex(licenseKey)) {
        result.allowed = false;
        result.online = false;
        result.reason = QStringLiteral("授权服务器响应与本次请求不匹配");
    } else if (!verifyFreshResponseSignature(result)) {
        result.allowed = false;
        result.online = false;
        result.reason = QStringLiteral("授权服务器响应签名无效");
    } else {
        result.online = true;
    }
    return result;
}

bool LicenseClient::saveCache(const QString &licenseKey, const QString &deviceId, const LicenseResult &result) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("cache_version"), kCacheFormatVersion);
    obj.insert(QStringLiteral("license_key"), licenseKey);
    obj.insert(QStringLiteral("product"), result.product.isEmpty() ? product() : result.product);
    obj.insert(QStringLiteral("device_id"), deviceId);
    obj.insert(QStringLiteral("key_hash"), result.keyHash);
    obj.insert(QStringLiteral("license_id"), result.licenseId);
    if (result.expiresAtIsNull) {
        obj.insert(QStringLiteral("expires_at"), QJsonValue::Null);
    } else {
        obj.insert(QStringLiteral("expires_at"), result.expiresAt);
    }
    obj.insert(QStringLiteral("last_success_server_time"), result.serverTime);
    obj.insert(QStringLiteral("last_success_local_time"), QDateTime::currentSecsSinceEpoch());
    obj.insert(QStringLiteral("server_sig"), result.responseSignature);
    obj.insert(QStringLiteral("cache_hmac"), cacheHmac(obj));

    QSaveFile file(cachePath());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return file.commit();
}

LicenseResult LicenseClient::tryCachedOffline() const {
    return tryCachedOffline(deviceFingerprint());
}

LicenseResult LicenseClient::tryCachedOffline(const QString &deviceId) const {
    LicenseResult result = tryCachedOfflineAtPath(cachePath(), deviceId);
    if (result.allowed) {
        return result;
    }

    const LicenseResult legacy = tryCachedOfflineAtPath(legacyCachePath(), deviceId);
    if (legacy.allowed) {
        return legacy;
    }
    if (!legacy.reason.isEmpty()) {
        return legacy;
    }
    return result;
}

LicenseResult LicenseClient::tryCachedOfflineAtPath(const QString &path, const QString &deviceId) const {
    const QJsonObject obj = readJsonFile(path);
    LicenseResult result;
    result.online = false;
    result.licenseId = obj.value(QStringLiteral("license_id")).toString();
    result.deviceId = obj.value(QStringLiteral("device_id")).toString();
    result.product = obj.value(QStringLiteral("product")).toString();
    result.keyHash = obj.value(QStringLiteral("key_hash")).toString();
    result.expiresAtIsNull = obj.value(QStringLiteral("expires_at")).isNull() ||
                             obj.value(QStringLiteral("expires_at")).isUndefined();
    result.expiresAt = result.expiresAtIsNull ? 0 : obj.value(QStringLiteral("expires_at")).toVariant().toLongLong();
    result.serverTime = obj.value(QStringLiteral("last_success_server_time")).toVariant().toLongLong();
    result.responseSignature = obj.value(QStringLiteral("server_sig")).toString();

    if (!hasValidCacheHmac(obj)) {
        result.reason = QStringLiteral("离线授权缓存签名无效");
        return result;
    }

    if (!isSupportedLicenseProduct(result.product)) {
        result.reason = QStringLiteral("没有可用的离线授权缓存");
        return result;
    }

    if (result.deviceId != deviceId) {
        result.reason = QStringLiteral("离线授权缓存不属于本机");
        return result;
    }

    const QString cachedKey = obj.value(QStringLiteral("license_key")).toString();
    if (result.keyHash != sha256Hex(cachedKey)) {
        result.reason = QStringLiteral("离线授权缓存与 license key 不匹配");
        return result;
    }

    LicenseResult signedResult;
    signedResult.allowed = true;
    signedResult.licenseId = result.licenseId;
    signedResult.deviceId = result.deviceId;
    signedResult.product = result.product;
    signedResult.keyHash = result.keyHash;
    signedResult.expiresAt = result.expiresAt;
    signedResult.expiresAtIsNull = result.expiresAtIsNull;
    signedResult.serverTime = result.serverTime;
    signedResult.responseSignature = result.responseSignature;
    if (!verifyResponseSignature(signedResult)) {
        result.reason = QStringLiteral("离线授权服务器签名无效");
        return result;
    }

    const qint64 lastLocal = obj.value(QStringLiteral("last_success_local_time")).toVariant().toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (lastLocal <= 0 || now < lastLocal) {
        result.reason = QStringLiteral("离线授权缓存时间异常");
        return result;
    }

    const qint64 graceSeconds = static_cast<qint64>(offlineGraceDays()) * 24 * 60 * 60;
    if (now - lastLocal > graceSeconds) {
        result.reason = QStringLiteral("离线授权已超过 %1 天宽限期").arg(offlineGraceDays());
        return result;
    }

    const qint64 estimatedServerNow = result.serverTime > 0 ? result.serverTime + (now - lastLocal) : now;
    if (!result.expiresAtIsNull && result.expiresAt > 0 && estimatedServerNow > result.expiresAt) {
        result.reason = QStringLiteral("授权已过期");
        return result;
    }

    result.allowed = true;
    return result;
}

void LicenseClient::clearCache() {
    QFile::remove(cachePath());
    QFile::remove(legacyCachePath());
}

}  // namespace ccv2
