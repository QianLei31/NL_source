#include "theme/theme_manager.h"
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>
#include <algorithm>
#include <cmath>

namespace ccv2 {

ThemeManager::ThemeManager(QObject *parent)
    : QObject(parent)
{
}

// === Legacy API ===

QStringList ThemeManager::themeNames() {
    return {
        QStringLiteral("dark"),
        QStringLiteral("soft-dark"),
        QStringLiteral("light"),
        QStringLiteral("midnight-indigo"),
    };
}

QString ThemeManager::normalizeThemeName(const QString &themeName)
{
    const QString trimmed = themeName.trimmed();
    const QString key = trimmed.toLower();
    if (key == QStringLiteral("soft-dark") ||
        key == QStringLiteral("soft dark") ||
        key == QStringLiteral("layered soft dark") ||
        key == QStringLiteral("claude dark") ||
        key == QStringLiteral("柔和暗色")) {
        return QStringLiteral("soft-dark");
    }
    if (key == QStringLiteral("light") ||
        key == QStringLiteral("light (lab gray)") ||
        key == QStringLiteral("lab light gray") ||
        key == QStringLiteral("clinical-white") ||
        key == QStringLiteral("clinical white") ||
        key == QStringLiteral("浅色主题") ||
        key == QStringLiteral("实验室浅色") ||
        key == QStringLiteral("临床白")) {
        return QStringLiteral("light");
    }
    if (key == QStringLiteral("dark") ||
        key == QStringLiteral("deep sea blue") ||
        key == QStringLiteral("dark (deep sea)") ||
        key == QStringLiteral("industrial teal") ||
        key == QStringLiteral("high contrast night") ||
        key == QStringLiteral("ink green") ||
        key == QStringLiteral("经典暗色")) {
        return QStringLiteral("dark");
    }
    if (key == QStringLiteral("midnight-indigo") ||
        key == QStringLiteral("midnight indigo") ||
        key == QStringLiteral("靛蓝夜色"))
        return QStringLiteral("midnight-indigo");
    if (key == QStringLiteral("green-phosphor") || key == QStringLiteral("green phosphor") ||
        key == QStringLiteral("绿色荧光") ||
        key == QStringLiteral("amber-lab") || key == QStringLiteral("amber lab") ||
        key == QStringLiteral("琥珀实验室") ||
        key == QStringLiteral("neon-cyber") || key == QStringLiteral("neon cyber") ||
        key == QStringLiteral("霓虹赛博")) {
        return QStringLiteral("dark");
    }
    return themeNames().contains(trimmed) ? trimmed : QStringLiteral("dark");
}

ThemeStyle ThemeManager::fromIndex(int idx) {
    switch (idx) {
        case 0: return ThemeStyle::DeepSeaBlue;
        case 1: return ThemeStyle::IndustrialTeal;
        case 2: return ThemeStyle::LabLightGray;
        case 3: return ThemeStyle::HighContrastNight;
        case 4: return ThemeStyle::InkGreen;
        default: return ThemeStyle::DeepSeaBlue;
    }
}

QString ThemeManager::styleSheetFor(ThemeStyle style) {
    // Legacy compat: return a minimal dark stylesheet for all styles
    // The new token-based system (apply()) should be used instead
    Q_UNUSED(style);
    return QStringLiteral(R"(
        QWidget { color: #d4e4f7; font-family: "Segoe UI", "Microsoft YaHei"; font-size: 13px; background: transparent; }
        QMainWindow { background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #06101e, stop:0.5 #0e2238, stop:1 #14314a); }
        QLabel#title { font-size: 30px; font-weight: 800; color: #f0f6ff; }
        QLabel#subtitle { color: #86a7c8; margin-bottom: 6px; }
        QGroupBox { background: rgba(14,30,52,0.85); border: 1px solid #1f3d5e; border-radius: 14px; margin-top: 14px; font-weight: 600; color: #c8ddf2; padding-top: 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 8px; }
        QListWidget#leftNav { background: #0d1b2f; border: 1px solid #27496e; border-radius: 10px; padding: 6px; min-width: 220px; max-width: 260px; outline: 0px; }
        QListWidget#leftNav::item { border-radius: 8px; padding: 12px 10px; margin: 4px 2px; background: #132740; }
        QListWidget#leftNav::item:selected { background: #1b4f7b; color: #f0f8ff; border: 1px solid #3f8cc8; }
        QLineEdit, QComboBox, QPlainTextEdit, QSpinBox, QDoubleSpinBox { background: #0a1628; color: #e4effc; border: 1px solid #2a4d72; border-radius: 8px; padding: 6px 10px; }
        QPushButton { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1565c0, stop:1 #0d47a1); color: #eaf2ff; border: 1px solid #1976d2; border-radius: 10px; padding: 7px 14px; font-weight: 600; }
        QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #1e88e5, stop:1 #1565c0); border-color: #42a5f5; }
        QScrollArea { border: none; }
    )");
}

// === New token-based API ===

void ThemeManager::apply(const QString &themeName)
{
    const QString canonicalTheme = normalizeThemeName(themeName);
    m_currentTheme = canonicalTheme;
    m_tokens = loadTokens(canonicalTheme);
    if (m_tokens.isEmpty() && canonicalTheme != QStringLiteral("dark")) {
        m_currentTheme = QStringLiteral("dark");
        m_tokens = loadTokens(m_currentTheme);
    }
    m_palette = buildPalette(m_tokens);

    // Load and concatenate QSS files in order: tokens (already parsed) + widgets + pages + plot
    QString widgetsQss = loadQssFile(QStringLiteral(":/theme/widgets.qss"));
    QString pagesQss = loadQssFile(QStringLiteral(":/theme/pages.qss"));
    QString plotQss = loadQssFile(QStringLiteral(":/theme/plot.qss"));

    QString combined = widgetsQss + QStringLiteral("\n") + pagesQss + QStringLiteral("\n") + plotQss;
    QString expanded = expand(combined, m_tokens);

    // Apply to application
    if (qApp) {
        qApp->setStyleSheet(expanded);
    }

    emit themeChanged(m_palette);
}

QString ThemeManager::expand(const QString &qss, const QMap<QString, QString> &tokens)
{
    QString result = qss;
    // Sort tokens by key length descending so longer tokens are replaced first
    // (e.g., @primary-press before @primary, @card-bg-elevated before @card-bg)
    QVector<QPair<QString, QString>> sorted;
    for (auto it = tokens.constBegin(); it != tokens.constEnd(); ++it) {
        sorted.append({it.key(), it.value()});
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.first.length() > b.first.length();
    });
    for (const auto &pair : sorted) {
        result.replace(pair.first, pair.second);
    }
    return result;
}

QVector<QColor> ThemeManager::seriesPalette(int n)
{
    QVector<QColor> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const qreal h = std::fmod(200.0 + i * 360.0 / std::max(1, n) * 0.62, 360.0);
        out.push_back(QColor::fromHslF(h / 360.0, 0.62, 0.58));
    }
    return out;
}

QMap<QString, QString> ThemeManager::loadTokens(const QString &themeName) const
{
    QMap<QString, QString> tokens;
    QString filename = QStringLiteral(":/theme/tokens-%1.qss").arg(themeName);
    QString content = loadQssFile(filename);

    // Parse lines like: @token-name: #hexvalue;
    // or: @token-name: 10px;
    static QRegularExpression re(QStringLiteral(R"((@[\w-]+)\s*:\s*([^;]+)\s*;)"));
    auto it = re.globalMatch(content);
    while (it.hasNext()) {
        auto match = it.next();
        tokens.insert(match.captured(1), match.captured(2).trimmed());
    }
    return tokens;
}

QString ThemeManager::loadQssFile(const QString &path) const
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        return stream.readAll();
    }
    qWarning() << "ThemeManager: failed to load QSS file:" << path;
    return QString();
}

ThemePalette ThemeManager::buildPalette(const QMap<QString, QString> &tokens) const
{
    ThemePalette p;
    auto color = [&](const QString &key, const QColor &fallback = Qt::black) -> QColor {
        if (tokens.contains(key)) return QColor(tokens.value(key));
        return fallback;
    };

    p.appBg = color(QStringLiteral("@app-bg"), QColor(0x06, 0x10, 0x1e));
    p.cardBg = color(QStringLiteral("@card-bg"), QColor(0x0e, 0x22, 0x38));
    p.cardBgElevated = color(QStringLiteral("@card-bg-elevated"), QColor(0x15, 0x30, 0x4b));
    p.cardBorder = color(QStringLiteral("@card-border"), QColor(0x1f, 0x3d, 0x5e));
    p.primary = color(QStringLiteral("@primary"), QColor(0x1e, 0x88, 0xe5));
    p.primaryHover = color(QStringLiteral("@primary-hover"), QColor(0x42, 0xa5, 0xf5));
    p.secondary = color(QStringLiteral("@secondary"), QColor(0x6a, 0x7e, 0xa0));
    p.success = color(QStringLiteral("@success"), QColor(0x2e, 0xcc, 0x71));
    p.warning = color(QStringLiteral("@warning"), QColor(0xf5, 0xa6, 0x23));
    p.error = color(QStringLiteral("@error"), QColor(0xe9, 0x45, 0x45));
    p.info = color(QStringLiteral("@info"), QColor(0x4a, 0xd6, 0xff));
    p.textPrimary = color(QStringLiteral("@text-primary"), QColor(0xf0, 0xf6, 0xff));
    p.textSecondary = color(QStringLiteral("@text-secondary"), QColor(0xa8, 0xc0, 0xdd));
    p.textDisabled = color(QStringLiteral("@text-disabled"), QColor(0x5d, 0x73, 0x8e));
    p.plotBg = color(QStringLiteral("@plot-bg"), QColor(0x08, 0x15, 0x24));
    p.plotGrid = color(QStringLiteral("@plot-grid"), QColor(0x1c, 0x33, 0x50));
    p.plotAxis = color(QStringLiteral("@plot-axis"), QColor(0x9e, 0xb8, 0xd5));
    p.plotWave = color(QStringLiteral("@plot-wave"), QColor(0x00, 0xe5, 0xff));

    return p;
}

}  // namespace ccv2
