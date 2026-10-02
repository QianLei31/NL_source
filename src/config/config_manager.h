#pragma once

#include <QMap>
#include <QString>

namespace ccv2 {

using ConfigSection = QMap<QString, QString>;
using ConfigMap = QMap<QString, ConfigSection>;

class ConfigManager {
public:
    explicit ConfigManager(const QString &fileName = QStringLiteral("NL_CommandCenter_v6_config.ini"));

    ConfigMap load() const;
    bool save(const ConfigMap &cfg) const;
    QString path() const;

private:
    QString m_fileName;

    static ConfigMap defaults();
};

}  // namespace ccv2
