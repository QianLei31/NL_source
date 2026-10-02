// Actual updater/settings and gated-license UI paths against loopback fixtures.
// This executable never contacts the production HTTP service, opens a browser,
// generates a signed license, or changes production endpoints/license policy.
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "config/config_manager.h"
#include "core/app_version.h"
#include "license/license_client.h"
#include "license/license_dialog.h"
#include "license/license_policy.h"
#include "service/session_hub.h"
#include "service/update_client.h"
#include "ui/command_center_main_window.h"
#include "ui/widgets/settings_panel.h"

using namespace ccv2;

namespace {
int checks = 0;
int failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void require(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void pump(int ms) {
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 6000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) pump(10);
    return condition();
}
QPushButton *button(QWidget *parent, const QString &text) {
    for (auto *b : parent->findChildren<QPushButton *>()) if (b->text() == text) return b;
    throw std::runtime_error(("Missing button: " + text).toStdString());
}
bool hasText(QWidget *parent, const QString &part) {
    for (auto *label : parent->findChildren<QLabel *>()) if (label->text().contains(part)) return true;
    return false;
}
int freePort() {
    QTcpServer server; server.setProxy(QNetworkProxy::NoProxy);
    require(server.listen(QHostAddress::LocalHost, 0), "allocate local Dummy port");
    return server.serverPort();
}

struct HttpRequest {
    QByteArray method;
    QUrl url;
    QByteArray body;
    QByteArray headers;
};
struct HttpResponse {
    int status{200};
    QByteArray body{R"({"latest_version":"99.0.0"})"};
    bool hold{false};
    bool drop{false};
    bool unsignedLicense{false};
};

// The fixture acts only as an HTTP proxy endpoint: absolute-form requests are
// recorded, never forwarded. All listeners and peers are explicitly loopback.
class LoopbackHttp : public QObject {
public:
    LoopbackHttp() {
        server.setProxy(QNetworkProxy::NoProxy);
        require(server.listen(QHostAddress::LocalHost, 0), "loopback HTTP fixture listening");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                socket->setParent(this);
                buffers.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    buffers.remove(socket); socket->deleteLater();
                });
            }
        });
    }
    ~LoopbackHttp() override {
        for (auto *socket : findChildren<QTcpSocket *>()) {
            disconnect(socket, nullptr, this, nullptr); socket->abort();
        }
    }
    QNetworkProxy proxy() const {
        return QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), server.serverPort());
    }
    void enqueue(const HttpResponse &response) { responses.enqueue(response); }
    void attach(UpdateClient *client) {
        auto *manager = client->findChild<QNetworkAccessManager *>();
        require(manager, "actual UpdateClient network manager found");
        manager->setProxy(proxy());
    }
    QList<HttpRequest> requests;
    int unexpected{0};
private:
    void read(QTcpSocket *socket) {
        auto it = buffers.find(socket); if (it == buffers.end()) return;
        it.value().append(socket->readAll());
        const int end = it.value().indexOf("\r\n\r\n"); if (end < 0) return;
        const QByteArray headers = it.value().left(end);
        int length = 0;
        for (const auto &line : headers.split('\n')) {
            if (line.trimmed().toLower().startsWith("content-length:"))
                length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        }
        if (it.value().size() < end + 4 + length) return;
        const auto requestLine = headers.left(headers.indexOf("\r\n")).split(' ');
        require(requestLine.size() >= 2, "fixture received parseable HTTP request");
        HttpRequest request{requestLine[0], QUrl(QString::fromUtf8(requestLine[1])),
                            it.value().mid(end + 4, length), headers};
        requests.append(request);
        buffers.erase(it); // one response/connection, always Connection: close
        HttpResponse response;
        if (responses.isEmpty()) {
            ++unexpected; response.status = 503; response.body = "{\"detail\":\"unexpected fixture request\"}";
        } else response = responses.dequeue();
        if (response.hold) return;
        if (response.drop) { socket->abort(); return; }
        if (response.unsignedLicense) {
            const auto sent = QJsonDocument::fromJson(request.body).object();
            QJsonObject denied;
            denied["valid"] = false;
            denied["reason"] = "Synthetic unsigned denial; not an authorization";
            denied["product"] = sent["product"];
            denied["device_id"] = sent["device_id"];
            denied["key_hash"] = QString::fromLatin1(QCryptographicHash::hash(
                sent["key"].toString().toUtf8(), QCryptographicHash::Sha256).toHex());
            denied["server_time"] = QDateTime::currentSecsSinceEpoch();
            // Intentionally no signature, no license_id, no valid=true fixture.
            response.body = QJsonDocument(denied).toJson(QJsonDocument::Compact);
        }
        const QByteArray reason = response.status == 200 ? "OK" : response.status == 404 ? "Not Found" : "Fixture Error";
        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " " + reason +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(response.body.size()) +
                      "\r\nConnection: close\r\n\r\n" + response.body);
        socket->disconnectFromHost();
    }
    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffers;
    QQueue<HttpResponse> responses;
};

class ScopedApplicationProxy {
public:
    explicit ScopedApplicationProxy(const QNetworkProxy &proxy) : previous(QNetworkProxy::applicationProxy()) {
        QNetworkProxy::setApplicationProxy(proxy);
    }
    ~ScopedApplicationProxy() { QNetworkProxy::setApplicationProxy(previous); }
private:
    QNetworkProxy previous;
};
}

class ReleaseCapture : public QObject {
    Q_OBJECT
public:
    ReleaseCapture() {
        for (const auto &scheme : schemes) QDesktopServices::setUrlHandler(scheme, this, "capture");
    }
    ~ReleaseCapture() override { for (const auto &scheme : schemes) QDesktopServices::unsetUrlHandler(scheme); }
    QList<QUrl> urls;
public slots:
    void capture(const QUrl &url) { urls.append(url); }
private:
    QStringList schemes{QStringLiteral("https"), QStringLiteral("http"), QStringLiteral("file"),
                        QStringLiteral("javascript"), QStringLiteral("data")};
};

namespace {
int lifetimeChild(const QString &mode) {
    LoopbackHttp http;
    auto client = std::make_unique<UpdateClient>(); http.attach(client.get());
    int completed = 0; UpdateCheckResult result;
    QObject::connect(client.get(), &UpdateClient::checkFinished, qApp, [&](const UpdateCheckResult &r) { ++completed; result = r; });
    http.enqueue({200, {}, true});
    client->checkForUpdates("1.0.0");
    require(waitFor([&] { return http.requests.size() == 1; }), "pending update reached loopback fixture");
    if (mode == "--repeat-check-child") {
        std::cout << "REPRO: replace pending UpdateClient request" << std::endl;
        http.enqueue({200, R"({"latest_version":"3.0.0"})"});
        client->checkForUpdates("2.0.0");
        require(waitFor([&] { return completed > 0; }), "replacement request terminates");
        check(completed == 1 && result.currentVersion == "2.0.0" && result.latestVersion == "3.0.0",
              "replaced request emits only the new result");
        pump(100);
        check(completed == 1, "replacement has no late old-result emission");
    } else {
        std::cout << "REPRO: destroy UpdateClient with pending request" << std::endl;
        client.reset(); pump(100);
        check(completed == 0, "destruction does not emit a spurious check result");
    }
    check(http.unexpected == 0, "lifetime fixture received only expected requests");
    return failures ? 1 : 0;
}
void runLifetimeSubprocess(const QString &argument) {
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath()); child.setArguments({argument});
    child.setProcessChannelMode(QProcess::MergedChannels); child.start();
    require(child.waitForStarted(5000), "lifetime subprocess started");
    const bool finished = child.waitForFinished(15000);
    if (!finished) { child.kill(); child.waitForFinished(); }
    std::cout << child.readAll().toStdString();
    check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
          argument == "--repeat-check-child" ? "AUX10 replace-pending lifetime regression" : "AUX11 destroy-pending lifetime regression");
    std::cout << "LIFETIME " << argument.toStdString() << " status=" << child.exitStatus() << " exit=" << child.exitCode() << '\n';
}

void updateUiTests() {
    const int initialFailures = failures;
    LoopbackHttp http;
    ReleaseCapture capture;
    ConfigManager configManager; auto config = configManager.load();
    config["Network"]["host"] = "127.0.0.1";
    config["Network"]["port"] = QString::number(freePort());
    config["Network"]["data_port"] = config["Network"]["port"];
    require(configManager.save(config), "isolated application configuration saved");
    CommandCenterMainWindow window;
    struct Close { CommandCenterMainWindow &window; ~Close() { window.close(); } } close{window};
    window.resize(1360, 940); window.show(); pump(100);
    auto *settings = window.findChild<SettingsPanel *>();
    auto *client = window.findChild<UpdateClient *>();
    auto *hub = window.findChild<SessionHub *>();
    require(settings && client && hub, "actual settings, updater and SessionHub found");
    http.attach(client);
    auto *openSettings = window.findChild<QPushButton *>("settingsIconButton");
    auto *checkButton = settings->findChild<QPushButton *>("settingsCheckUpdateButton");
    auto *openRelease = settings->findChild<QPushButton *>("settingsOpenUpdateButton");
    auto *status = settings->findChild<QLabel *>("settingsUpdateStatus");
    require(openSettings && checkButton && openRelease && status, "actual update controls found");
    int completed = 0; UpdateCheckResult result;
    QObject::connect(client, &UpdateClient::checkFinished, &window, [&](const UpdateCheckResult &r) { ++completed; result = r; });

    // Start the real built-in acquisition Dummy through the actual settings GUI.
    int control = freePort(), data = freePort(); while (data == control) data = freePort();
    settings->setLocalTestPorts(control, data); settings->setManagedTestSource(true);
    settings->findChild<QCheckBox *>("settingsLocalToggle")->setChecked(true);
    settings->show(); settings->findChild<QPushButton *>("settingsConnectButton")->click();
    require(waitFor([&] { return hub->isConnected() && hub->statistics().distributedFrames > 200; }),
            "actual built-in Dummy acquisition active during updater GUI checks");
    const auto startFrames = hub->statistics().distributedFrames;

    const QString trusted = "https://github.com/QianLei31/NL_CommandCenter_v5_release/releases/tag/v99.0.0";
    http.enqueue({200, QJsonDocument(QJsonObject{{"latest_version", "99.0.0"}, {"download_url", trusted},
                                                {"message", "Local fixture release"}}).toJson()});
    openSettings->click();
    check(!checkButton->isEnabled() && status->text().contains("正在检查"), "AUX01 first settings-open starts check and disables repeat click");
    const int seenBeforeDisabledClick = http.requests.size(); checkButton->click();
    require(waitFor([&] { return completed == 1; }), "initial automatic update completes");
    check(http.requests.size() == seenBeforeDisabledClick + 1, "disabled duplicate check creates no second request");
    check(result.online && result.updateAvailable && result.latestVersion == "99.0.0" && result.downloadUrl == trusted,
          "AUX01 actual parser returns update metadata");
    check(checkButton->isEnabled() && status->property("state").toString() == "ok" && status->text().contains("Local fixture release"),
          "AUX01 update success appears in settings");
    openRelease->click();
    check(capture.urls.size() == 1 && capture.urls.last() == QUrl(trusted), "AUX02 actual open-release click captured trusted target without launching browser");
    const auto firstRequest = http.requests.first();
    QUrlQuery query(firstRequest.url);
    check(firstRequest.method == "GET" && firstRequest.url.path() == "/v1/app-version" &&
          query.queryItemValue("product") == LicenseClient::product() &&
          query.queryItemValue("current_version") == appVersionDisplay() && query.queryItemValue("channel") == "stable" &&
          firstRequest.headers.contains("NL_CommandCenter/"), "AUX01 request path/product/version/channel/user-agent match production protocol");
    button(settings, QStringLiteral("关闭"))->click(); openSettings->click(); pump(50);
    check(completed == 1 && http.requests.size() == 1, "AUX03 reopen settings does not repeat automatic session check");

    auto run = [&](const HttpResponse &response) {
        const int before = completed; http.enqueue(response); checkButton->click();
        check(!checkButton->isEnabled(), "check action enters pending UI state");
        require(waitFor([&] { return completed == before + 1; }, 7500), "update fixture completed exactly once");
        check(checkButton->isEnabled() && openRelease->isEnabled(), "finished update restores both actions");
        return result;
    };
    auto r = run({200, QJsonDocument(QJsonObject{{"latest_version", appVersionDisplay()}}).toJson()});
    check(r.online && !r.updateAvailable && status->text().contains("当前已是最新版本") && status->property("state").toString() == "info",
          "AUX04 no-update result reaches actual settings UI");
    r = run({200, "{\"latest_version\":\"7.0.0\"}"});
    check(r.online && r.updateAvailable && status->text().contains("发现新版本 7.0.0"),
          "AUX04 stable 7.0.0 must upgrade the shipped preview build");
    const QString currentPreview=QString::fromLatin1(kAppVersion);
    const int previewMarker=currentPreview.indexOf(QStringLiteral("-preview."));
    const int nextNumber=currentPreview.mid(previewMarker+9).toInt()+1;
    const QString nextPreview=currentPreview.left(previewMarker+9)+QString::number(nextNumber);
    r = run({200, QJsonDocument(QJsonObject{{"latest_version",nextPreview}}).toJson()});
    check(previewMarker>0 && r.online && r.updateAvailable && status->text().contains(nextPreview),
          "AUX04 next preview must upgrade the shipped preview regardless of appended build date");
    check(UpdateClient::compareVersions("6.0.4-20260624", "6.0.4-20260623") > 0 &&
          UpdateClient::compareVersions("v6.0.4-20260623", "6.0.4-20260623") == 0,
          "AUX04 legacy dated 6.x build ordering and v prefix remain compatible");
    r = run({404, "{\"detail\":\"no version published\"}"});
    check(r.online && !r.updateAvailable && r.error.isEmpty() && status->text().contains("暂未发布版本信息"),
          "AUX05 HTTP 404 is reachable/no published release, not a false offline failure");
    r = run({503, "{}"});
    check(!r.online && r.error.contains("HTTP 503") && status->text().contains("失败") && status->property("state").toString() == "warn",
          "AUX06 HTTP server error visible with retry enabled");
    r = run({200, "<html>not-json</html>"});
    check(!r.online && r.error.contains("格式错误"), "AUX06 malformed JSON fails closed");
    r = run({200, "[]"}); check(!r.online && r.error.contains("格式错误"), "AUX06 non-object JSON rejected");
    r = run({200, "{\"message\":\"missing version\"}"});
    check(!r.online && r.error.contains("latest_version"), "AUX06 missing version field rejected");
    r = run({200, R"({"tag_name":"v99.0.1","release_url":"https://github.com/example/releases","notes":"alias message","update_available":false})"});
    check(r.online && !r.updateAvailable && r.latestVersion == "v99.0.1" && r.message == "alias message" && status->text() == "alias message",
          "AUX07 supported aliases and explicit no-update flag honored");
    for (const auto &candidate : QStringList{"file:///tmp/must-not-open", "javascript:alert(1)", "data:text/html,fixture",
             "http://github.com/example/release", "https://github.com.evil.invalid/release", "https://github.com@evil.invalid/release", "not a url"}) {
        r = run({200, QJsonDocument(QJsonObject{{"version", "99.0.0"}, {"url", candidate}}).toJson()});
        const auto before = capture.urls.size(); openRelease->click();
        check(r.downloadUrl == UpdateClient::defaultDownloadUrl() && capture.urls.size() == before + 1 &&
              capture.urls.last() == QUrl(UpdateClient::defaultDownloadUrl()), "AUX08 untrusted scheme/host sanitizes to built-in HTTPS release target");
    }
    for (const auto &candidate : QStringList{"https://releases.github.com/local-fixture", "https://project.github.io/releases"}) {
        r = run({200, QJsonDocument(QJsonObject{{"tag", "99.0.0"}, {"github_url", candidate}}).toJson()});
        openRelease->click();
        check(r.downloadUrl == candidate && capture.urls.last() == QUrl(candidate), "AUX08 trusted GitHub subdomain release URLs retained");
    }
    check(UpdateClient::compareVersions("v6.0.1", "6.0.1.0") == 0 &&
          UpdateClient::compareVersions("6.0.2", "6.0.1") > 0 && UpdateClient::compareVersions("5.9", "6.0") < 0,
          "AUX07 numeric version comparison handles v prefix and trailing zero components");

    const struct { const char *left; const char *right; int order; } versionCases[] = {
        {"7.0.0", "7.0.0-preview.1-20261002", 1},
        {"7.0.0-preview.1-20261002", "7.0.0", -1},
        {"v7.0.0-20261003", "7.0.0-preview.2-20261004", 1},
        {"7.0.0-preview.2", "7.0.0-preview.1-20261002", 1},
        {"7.0.0-preview.1-20261003", "7.0.0-preview.1-20261002", 1},
        {"7.0.1-preview.1", "7.0.0-20261002", 1},
        {"7.0.0-preview.1", "7.0.1", -1},
        {"6.0.4-20260624", "6.0.4-20260623", 1},
        {"v6.0.4-20260623", "6.0.4-20260623", 0},
        {"v6.0.1", "6.0.1.0", 0},
        {"7.0.0-unknown.2", "7.0.0-unknown.1", 1},
        {"7.0.0-unknown.1", "7.0.0", 1}, // existing fallback retained
    };
    for (const auto &entry : versionCases) {
        const int value = UpdateClient::compareVersions(entry.left, entry.right);
        const int order = (value > 0) - (value < 0);
        if (order != entry.order) std::cerr << "VERSION: " << entry.left << " vs " << entry.right << '\n';
        check(order == entry.order, "AUX07 preview/stable/date compatibility comparison table");
    }

    // A held real QNetworkReply exercises the production 5000-ms timeout.
    const int beforeTimeout = completed, requestsBeforeTimeout = http.requests.size();
    http.enqueue({200, {}, true}); checkButton->click();
    require(waitFor([&] { return http.requests.size() == requestsBeforeTimeout + 1; }, 1000), "pending timeout fixture was sent");
    button(settings, QStringLiteral("关闭"))->click();
    check(!settings->isVisible(), "AUX09 pending update can be dismissed with settings close");
    require(waitFor([&] { return completed == beforeTimeout + 1; }, 7000), "AUX09 actual update timeout completed");
    pump(150);
    check(completed == beforeTimeout + 1 && !result.online && result.error.contains("超时"), "AUX09 timeout abort emits only one failure result");
    openSettings->click();
    check(checkButton->isEnabled() && status->text().contains("超时"), "AUX09 reopening shows completed timeout and allows retry");

    // Explicit refused local proxy endpoint proves transport failure without any
    // DNS request or chance of reaching the real service.
    QTcpServer refused; refused.setProxy(QNetworkProxy::NoProxy); require(refused.listen(QHostAddress::LocalHost, 0), "reserve refused proxy port");
    auto *manager = client->findChild<QNetworkAccessManager *>();
    manager->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", refused.serverPort())); refused.close();
    const int beforeError = completed; checkButton->click();
    require(waitFor([&] { return completed == beforeError + 1; }), "refused loopback transport error completes");
    check(!result.online && !result.error.isEmpty() && checkButton->isEnabled(), "AUX09 network error is recoverable in actual settings UI");
    http.attach(client); r = run({200, "{\"latest_version\":\"99.0.0\"}"});
    check(r.online && r.updateAvailable, "AUX09 retry after timeout/network/parser/HTTP failures succeeds");
    check(hub->isConnected() && hub->statistics().distributedFrames > startFrames, "updater/error/retry work leaves acquisition Dummy receiving");
    check(http.unexpected == 0, "all updater requests handled only by explicit loopback fixtures");
    std::cout << "AUX01-AUX09 " << (failures == initialFailures ? "PASS" : "FAIL")
              << ": actual settings/updater with receiving Dummy; HTTP requests=" << http.requests.size() << '\n';
    window.close(); pump(100);
}

void licenseUiTests(const QString &dataHome) {
    LoopbackHttp http;
    // LicenseClient's manager is stack-local; this proxy is private to this
    // test executable and scope, not an operating-system/network setting.
    ScopedApplicationProxy proxy(http.proxy());
    const QString current = QDir(dataHome).filePath("NL_CommandCenter_v5_qt");
    QDir().mkpath(current);
    QFile fingerprint(QDir(current).filePath("device_fingerprint.txt"));
    require(fingerprint.open(QIODevice::WriteOnly), "isolated synthetic device fingerprint file");
    fingerprint.write(QByteArray(64, 'a')); fingerprint.close();
    const QString cache = QDir(current).filePath("license_cache.json");
    LicenseClient service;
    check(service.cachedLicenseKey().isEmpty() && !service.tryCachedOffline().allowed, "AUX12 empty isolated cache cannot authorize offline");
    check(isLicenseGracePeriod(QDate(2026, 12, 21)) && !isLicenseGracePeriod(QDate(2026, 12, 22)) &&
          licenseRequiredFromDate() == QDate(2026, 12, 22), "AUX12 documented grace boundary tested without changing clock or policy");
    LicenseDialog dialog; dialog.show(); pump(50);
    auto *key = dialog.findChild<QLineEdit *>();
    auto *verify = button(&dialog, QStringLiteral("校验并进入"));
    require(key, "actual license key editor");
    key->setText("   "); verify->click();
    check(dialog.isVisible() && dialog.result() != QDialog::Accepted && verify->isEnabled() &&
          hasText(&dialog, "请输入 license key") && http.requests.isEmpty(), "AUX13 empty license fails locally and can be corrected");
    const QString oldClipboard = QApplication::clipboard()->text();
    button(&dialog, QStringLiteral("复制"))->click();
    check(QApplication::clipboard()->text() == QString(64, 'a'), "AUX13 fingerprint copy actual button uses isolated fixture identity");
    QApplication::clipboard()->setText(oldClipboard);
    key->setText("  DUMMY-INVALID-NOT-A-LICENSE  ");
    auto run = [&](const HttpResponse &response, const QString &expected) {
        const int before = http.requests.size(); http.enqueue(response); verify->click();
        check(http.requests.size() == before + 1, "one license attempt sends one local fixture request");
        check(dialog.isVisible() && dialog.result() != QDialog::Accepted && verify->isEnabled() && hasText(&dialog, expected),
              "license rejection stays in dialog with specific error and enabled retry");
        check(!QFile::exists(cache), "rejected/unsigned/offline fixture never saves an authorization cache");
    };
    run({403, "{\"detail\":\"Synthetic invalid key\"}"}, "Synthetic invalid key");
    const auto request = http.requests.last();
    const auto body = QJsonDocument::fromJson(request.body).object();
    check(request.method == "POST" && request.url.path() == "/v1/verify" &&
          body["key"].toString() == "DUMMY-INVALID-NOT-A-LICENSE" && body["product"].toString() == LicenseClient::product() &&
          body["device_id"].toString() == QString(64, 'a'), "AUX14 exact license request protocol uses trimmed synthetic key/product/device");
    run({503, "{\"detail\":\"Synthetic server offline\"}"}, "Synthetic server offline");
    run({200, "<html>offline captive portal fixture</html>"}, "返回格式错误");
    run({200, "{\"valid\":false,\"reason\":\"unsigned mismatch\"}"}, "本次请求不匹配");
    run({200, {}, false, false, true}, "响应签名无效");
    QTcpServer refused; refused.setProxy(QNetworkProxy::NoProxy);
    require(refused.listen(QHostAddress::LocalHost, 0), "reserve refused local license proxy port");
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", refused.serverPort()));
    refused.close();
    const int beforeRefused = http.requests.size(); verify->click();
    check(http.requests.size() == beforeRefused && dialog.isVisible() && dialog.result() != QDialog::Accepted &&
          verify->isEnabled() && hasText(&dialog, "授权失败") && !QFile::exists(cache),
          "AUX14 real transport refusal cannot authorize without signed offline cache");
    QNetworkProxy::setApplicationProxy(http.proxy());
    std::cout << "AUX12-AUX14 PASS: empty/invalid/offline/transport/malformed/mismatch/unsigned-signature license paths; no activation\n";

    // Observe the real modal progress and full 8000-ms timeout. There is no
    // user-visible Cancel control in the production verification progress.
    bool progressSeen = false, progressHasCancel = false, verifyDisabled = false;
    QTimer observer; observer.setInterval(20);
    QObject::connect(&observer, &QTimer::timeout, &dialog, [&] {
        for (auto *p : dialog.findChildren<QProgressDialog *>()) {
            if (!p->isVisible()) continue;
            progressSeen = true; verifyDisabled = !verify->isEnabled();
            for (auto *b : p->findChildren<QPushButton *>()) if (b->isVisible()) progressHasCancel = true;
        }
    });
    observer.start(); run({200, {}, true}, "连接授权服务器超时"); observer.stop();
    check(progressSeen && verifyDisabled && !progressHasCancel, "AUX15 actual pending license progress is modal, verify disabled, no offered cancel button");
    run({403, "{\"detail\":\"Synthetic retry rejected\"}"}, "Synthetic retry rejected");
    button(&dialog, QStringLiteral("退出"))->click();
    check(!dialog.isVisible() && dialog.result() == QDialog::Rejected, "AUX16 actual exit rejects license dialog after failed/repeated checks");
    LicenseDialog escaped; escaped.show(); QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&escaped, &escape);
    check(!escaped.isVisible() && escaped.result() == QDialog::Rejected, "AUX16 Escape cancels idle license dialog");
    LicenseDialog closed; closed.show(); closed.close();
    check(!closed.isVisible() && closed.result() == QDialog::Rejected, "AUX16 window close rejects idle license dialog");
    check(http.unexpected == 0 && !QFile::exists(cache), "license fixtures never authorize, never save license cache, never request live service");
    std::cout << "AUX15-AUX16 PASS: production timeout, retry, exit/Escape/window-close; signed success BLOCKED (no authorized signed fixture)\n";
}
}

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    std::cout.setf(std::ios::unitbuf);
    app.setOrganizationName("NeuralLabDummyQa"); app.setApplicationName("AuxiliaryUiE2E");
    try {
        QTemporaryDir temporary; require(temporary.isValid(), "temporary test directory");
        qputenv("XDG_CONFIG_HOME", temporary.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", temporary.filePath("data").toUtf8());
        if (app.arguments().contains("--repeat-check-child")) return lifetimeChild("--repeat-check-child");
        if (app.arguments().contains("--destroy-pending-child")) return lifetimeChild("--destroy-pending-child");
        updateUiTests();
        runLifetimeSubprocess("--repeat-check-child");
        runLifetimeSubprocess("--destroy-pending-child");
        licenseUiTests(temporary.filePath("data"));
    } catch (const std::exception &error) { std::cerr << "FATAL: " << error.what() << '\n'; return 2; }
    std::cout << "dummy_auxiliary_ui_e2e " << (failures ? "FAILED" : "OK") << " checks=" << checks << " failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

#include "dummy_auxiliary_ui_e2e.moc"
