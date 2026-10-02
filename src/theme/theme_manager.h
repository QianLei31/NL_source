#pragma once

#include <QString>
#include <QStringList>
#include <QColor>
#include <QVector>
#include <QMap>
#include <QObject>

namespace ccv2 {

/// Color palette extracted from token QSS for programmatic use (plots, maps, etc.)
struct ThemePalette {
    QColor appBg;
    QColor cardBg;
    QColor cardBgElevated;
    QColor cardBorder;
    QColor primary;
    QColor primaryHover;
    QColor secondary;
    QColor success;
    QColor warning;
    QColor error;
    QColor info;
    QColor textPrimary;
    QColor textSecondary;
    QColor textDisabled;
    QColor plotBg;
    QColor plotGrid;
    QColor plotAxis;
    QColor plotWave;
};

enum class ThemeStyle {
    DeepSeaBlue = 0,   // maps to "dark" tokens
    IndustrialTeal,
    LabLightGray,      // maps to "light" tokens
    HighContrastNight,
    InkGreen
};

class ThemeManager : public QObject {
    Q_OBJECT
public:
    explicit ThemeManager(QObject *parent = nullptr);

    // === Legacy API (backward compat) ===
    static QStringList themeNames();
    static QString normalizeThemeName(const QString &themeName);
    static ThemeStyle fromIndex(int idx);
    static QString styleSheetFor(ThemeStyle style);  // legacy compat

    // === New token-based API ===
    void apply(const QString &themeName);  // "dark", "soft-dark", or "light"
    QString currentTheme() const { return m_currentTheme; }
    ThemePalette currentPalette() const { return m_palette; }

    /// Generate N harmonious colors for multi-channel waveforms
    static QVector<QColor> seriesPalette(int n);

    /// Expand @token references in a QSS string using the given token map
    static QString expand(const QString &qss, const QMap<QString, QString> &tokens);

signals:
    void themeChanged(const ThemePalette &palette);

private:
    QMap<QString, QString> loadTokens(const QString &themeName) const;
    QString loadQssFile(const QString &path) const;
    ThemePalette buildPalette(const QMap<QString, QString> &tokens) const;

    QString m_currentTheme;
    ThemePalette m_palette;
    QMap<QString, QString> m_tokens;
};

}  // namespace ccv2
