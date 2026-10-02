#include "license/device_fingerprint.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>

namespace ccv2 {

namespace {

QString runPowerShell(const QString &command) {
    QProcess process;
    process.start(QStringLiteral("powershell.exe"),
                  {QStringLiteral("-NoProfile"),
                   QStringLiteral("-ExecutionPolicy"),
                   QStringLiteral("Bypass"),
                   QStringLiteral("-Command"),
                   command});
    if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
}

QStringList nonEmptyLines(const QString &text) {
    QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                   Qt::SkipEmptyParts);
    for (QString &line : lines) {
        line = line.trimmed();
    }
    return lines;
}

// Same directory the license cache lives in (see license_client.cpp).
QString fingerprintCachePath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString dir = base.isEmpty()
                      ? QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                            .filePath(QStringLiteral(".nl_command_center_v5"))
                      : QDir(base).filePath(QStringLiteral("NL_CommandCenter_v5_qt"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("device_fingerprint.txt"));
}

bool looksLikeFingerprint(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar ch : value) {
        if (!ch.isDigit() && !(ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

// fromCim reports whether the hash came from the stable CIM triplet (the
// source every historical activation used) rather than a fallback id.
QString computeFingerprint(bool *fromCim) {
    const QStringList values = nonEmptyLines(runPowerShell(QStringLiteral(
        "$cpu=(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty ProcessorId);"
        "$board=(Get-CimInstance Win32_BaseBoard | Select-Object -First 1 -ExpandProperty SerialNumber);"
        "$guid=(Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Cryptography').MachineGuid;"
        "Write-Output $cpu; Write-Output $board; Write-Output $guid")));
    const QString cpu = values.value(0);
    const QString board = values.value(1);
    const QString machineGuid = values.value(2);

    QString raw = cpu + QStringLiteral("|") + board + QStringLiteral("|") + machineGuid;
    bool cimOk = raw != QStringLiteral("||");
    if (!cimOk) {
        raw = QSysInfo::machineUniqueId();
    }
    if (raw.trimmed().isEmpty()) {
        raw = QSysInfo::machineHostName() + QStringLiteral("|") + QSysInfo::prettyProductName();
        cimOk = false;
    }
    if (fromCim) {
        *fromCim = cimOk;
    }

    return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex());
}

}  // namespace

QString deviceFingerprint() {
    // The CIM query behind computeFingerprint() runs PowerShell with a 3s
    // timeout; under AV scans or heavy load it can intermittently fail and
    // fall back to a different id source, which would invalidate the license
    // cache ("cache not for this machine") and online device binding. Persist
    // the first computed fingerprint and reuse it so this machine's identity
    // never flips between sources.
    const QString path = fingerprintCachePath();
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QString cached = QString::fromLatin1(file.readAll()).trimmed();
        if (looksLikeFingerprint(cached)) {
            return cached;
        }
    }

    // Persist only a CIM-derived fingerprint: pinning a fallback value (from
    // a one-off PowerShell timeout) would permanently mismatch the identity
    // that historical activations and caches were bound to.
    bool fromCim = false;
    const QString computed = computeFingerprint(&fromCim);
    if (fromCim) {
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(computed.toLatin1());
            out.commit();
        }
    }
    return computed;
}

}  // namespace ccv2
