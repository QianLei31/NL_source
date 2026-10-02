#include "config/config_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace ccv2 {

namespace {

QString bundledConfigPath(const QString &fileName)
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(fileName);
}

QString writableConfigPath(const QString &fileName)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (!base.isEmpty()) {
            base = QDir(base).filePath(QStringLiteral("NL_CommandCenter_v6"));
        }
    }
    if (base.isEmpty()) {
        return bundledConfigPath(fileName);
    }

    QDir dir(base);
    dir.mkpath(QStringLiteral("."));
    return dir.filePath(fileName);
}

}  // namespace

ConfigManager::ConfigManager(const QString &fileName) : m_fileName(fileName) {}

QString ConfigManager::path() const {
    return writableConfigPath(m_fileName);
}

ConfigMap ConfigManager::defaults() {
    ConfigMap cfg;
    cfg[QStringLiteral("Network")] = {
        {QStringLiteral("host"), QStringLiteral("192.168.3.20")},
        {QStringLiteral("port"), QStringLiteral("7")},
        {QStringLiteral("data_port"), QStringLiteral("5001")}
    };
    cfg[QStringLiteral("LocalTest")] = {
        {QStringLiteral("source"), QStringLiteral("external")},
        {QStringLiteral("waveform"), QStringLiteral("sine")},
        {QStringLiteral("control_port"), QStringLiteral("10086")},
        {QStringLiteral("data_port"), QStringLiteral("10086")},
    };
    cfg[QStringLiteral("Signal")] = {
        {QStringLiteral("sampling_rate"), QStringLiteral("20000")},
        {QStringLiteral("refresh_hz"), QStringLiteral("10")},
        {QStringLiteral("wave_points"), QStringLiteral("8000")},
        {QStringLiteral("fft_enabled"), QStringLiteral("1")},
        {QStringLiteral("fft_points"), QStringLiteral("2048")},
        {QStringLiteral("fft_window"), QStringLiteral("hann")},
        {QStringLiteral("fft_dc_bins"), QStringLiteral("5")},
        {QStringLiteral("fft_sig_bins"), QStringLiteral("5")},
        {QStringLiteral("fft_bandwidth_hz"), QStringLiteral("10000")},
        {QStringLiteral("irn_gain"), QStringLiteral("60")},
    };
    cfg[QStringLiteral("Realtime")] = {
        {QStringLiteral("command"), QStringLiteral("ctre")},
        {QStringLiteral("channels"), QStringLiteral("0,8,16,24,32,40,48,56,64,72,80,88,96,104,112,120,128,136,144,152,160,168,176,184,192,200,208,216,224,232,240,248")},
        {QStringLiteral("wave_points"), QStringLiteral("1200")},
        {QStringLiteral("refresh_hz"), QStringLiteral("10")},
        {QStringLiteral("view_mode"), QStringLiteral("stack")},
        {QStringLiteral("signal_mode"), QStringLiteral("dc")},
        {QStringLiteral("stack_y_mode"), QStringLiteral("fixed")},
        {QStringLiteral("heatmap_metric"), QStringLiteral("rms")},
    };
    cfg[QStringLiteral("Analyzer")] = {
        {QStringLiteral("channels"), QStringLiteral("239,240")},
        {QStringLiteral("stats_source"), QString()},
        {QStringLiteral("pause_keep_capture"), QStringLiteral("1")},
    };
    // Application-wide TDM state (one switch + 0/2 versus 1/3 display pair).
    cfg[QStringLiteral("Session")] = {
        {QStringLiteral("tdm_enabled"), QStringLiteral("0")},
        {QStringLiteral("tdm_phase"), QStringLiteral("0")},
    };
    cfg[QStringLiteral("Sweep")] = {
        {QStringLiteral("channels"), QStringLiteral("0-7")},
        {QStringLiteral("command"), QStringLiteral("ctre")},
        {QStringLiteral("time_span"), QStringLiteral("4")},
        {QStringLiteral("sampling_rate"), QStringLiteral("20000")},
        {QStringLiteral("y_full_scale"), QStringLiteral("0.5")},
        {QStringLiteral("refresh_hz"), QStringLiteral("40")},
        {QStringLiteral("band"), QStringLiteral("1")},
        {QStringLiteral("notch_hz"), QStringLiteral("50")},
        {QStringLiteral("highpass_hz"), QStringLiteral("250")},
        {QStringLiteral("spike_lowpass_hz"), QStringLiteral("6000")},
        {QStringLiteral("threshold_mode"), QStringLiteral("0")},
        {QStringLiteral("threshold_value"), QStringLiteral("4")},
        {QStringLiteral("negative_polarity"), QStringLiteral("1")},
        {QStringLiteral("heatmap_metric"), QStringLiteral("0")},
        {QStringLiteral("click_mode"), QStringLiteral("0")},
    };
    cfg[QStringLiteral("SpikePanel")] = {
        {QStringLiteral("input_gain"), QStringLiteral("60")},
        {QStringLiteral("threshold_mode"), QStringLiteral("0")},
        {QStringLiteral("threshold_value"), QStringLiteral("4")},
        {QStringLiteral("polarity"), QStringLiteral("1")},
        {QStringLiteral("highpass_hz"), QStringLiteral("250")},
        {QStringLiteral("notch_hz"), QStringLiteral("0")},
        {QStringLiteral("pre_ms"), QStringLiteral("0.4")},
        {QStringLiteral("post_ms"), QStringLiteral("1.2")},
        {QStringLiteral("retain"), QStringLiteral("200")},
        {QStringLiteral("y_scale_uv"), QStringLiteral("200")},
        {QStringLiteral("auto_scale"), QStringLiteral("0")},
        {QStringLiteral("fade"), QStringLiteral("1")},
        {QStringLiteral("columns"), QStringLiteral("0")},
        {QStringLiteral("detail_y_scale_uv"), QStringLiteral("200")},
        {QStringLiteral("detail_auto_scale"), QStringLiteral("0")},
        {QStringLiteral("detail_fade"), QStringLiteral("1")},
        {QStringLiteral("detail_overlay_count"), QStringLiteral("100")},
        {QStringLiteral("threshold_overrides_adc"), QString()},
        {QStringLiteral("threshold_overrides_tdm"), QString()},
    };
    cfg[QStringLiteral("Paths")] = {{QStringLiteral("save_dir"), QStringLiteral("d:/ADC_data")}};
    cfg[QStringLiteral("Recording")] = {
        {QStringLiteral("save_dir"), QStringLiteral("d:/ADC_data")},
        {QStringLiteral("session_name"), QString()},
        {QStringLiteral("split_bytes"), QString::number(2LL << 30)},
        {QStringLiteral("warn_free_space_gb"), QStringLiteral("20")},
    };
    cfg[QStringLiteral("UI")] = {
        {QStringLiteral("theme"), QStringLiteral("dark")},
        {QStringLiteral("window_width"), QStringLiteral("1920")},
        {QStringLiteral("window_height"), QStringLiteral("1100")},
    };
    cfg[QStringLiteral("Stimulator")] = {
        {QStringLiteral("block"), QStringLiteral("00000000")},
        {QStringLiteral("addr_channel"), QStringLiteral("00")},
        {QStringLiteral("amplitude"), QStringLiteral("000000000")},
        {QStringLiteral("polarity"), QStringLiteral("00 (Output 0)")},
        {QStringLiteral("compensate"), QStringLiteral("0 (Disable)")},
        {QStringLiteral("step"), QStringLiteral("0 (4nA)")},
        {QStringLiteral("dac_channel"), QStringLiteral("00")},
    };
    cfg[QStringLiteral("Spi")] = {
        {QStringLiteral("direct_command"), QString()}
    };
    cfg[QStringLiteral("SpiQuickCommands")] = {
        {QStringLiteral("quick_commands_version"), QStringLiteral("2")},
        {QStringLiteral("count"), QStringLiteral("2")},
        {QStringLiteral("name_0"), QStringLiteral("增益全高")},
        {QStringLiteral("commands_0"), QStringLiteral("@gain_all_high")},
        {QStringLiteral("name_1"), QStringLiteral("增益全低")},
        {QStringLiteral("commands_1"), QStringLiteral("@gain_all_low")},
    };
    cfg[QStringLiteral("ChannelMap")] = {
        {QStringLiteral("block_range"), QStringLiteral("0-63")},
        {QStringLiteral("locals"), QStringLiteral("0,1,2,3")},
        {QStringLiteral("global_channels"), QString()},
        {QStringLiteral("routed_only"), QStringLiteral("1")},
        {QStringLiteral("zoom_percent"), QStringLiteral("100")},
    };
    cfg[QStringLiteral("TimestampCheck")] = {
        {QStringLiteral("duration_seconds"), QStringLiteral("60")},
        {QStringLiteral("expected_step"), QStringLiteral("1")},
    };
    return cfg;
}

bool ConfigManager::save(const ConfigMap &cfg) const {
    QFileInfo info(path());
    QDir().mkpath(info.absolutePath());

    QSettings settings(path(), QSettings::IniFormat);
    for (auto secIt = cfg.cbegin(); secIt != cfg.cend(); ++secIt) {
        settings.beginGroup(secIt.key());
        settings.remove(QString());
        for (auto kvIt = secIt.value().cbegin(); kvIt != secIt.value().cend(); ++kvIt) {
            settings.setValue(kvIt.key(), kvIt.value());
        }
        settings.endGroup();
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

ConfigMap ConfigManager::load() const {
    QString loadPath = path();
    const QString bundledPath = bundledConfigPath(m_fileName);
    const QString legacyPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.ini"));
    bool migrateLegacy = false;

    if (!QFileInfo::exists(loadPath)) {
        if (QFileInfo::exists(bundledPath)) {
            loadPath = bundledPath;
            migrateLegacy = true;
        } else if (m_fileName != QStringLiteral("config.ini") && QFileInfo::exists(legacyPath)) {
            loadPath = legacyPath;
            migrateLegacy = true;
        } else {
            save(defaults());
        }
    }

    if (!QFileInfo::exists(loadPath)) {
        save(defaults());
        loadPath = path();
    }

    QSettings settings(loadPath, QSettings::IniFormat);
    ConfigMap cfg;
    const QStringList groups = settings.childGroups();
    for (const QString &group : groups) {
        settings.beginGroup(group);
        ConfigSection sec;
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            sec[key] = settings.value(key).toString();
        }
        settings.endGroup();
        cfg[group] = sec;
    }

    const ConfigMap def = defaults();
    bool changed = false;

    // One-time migration: TDM used to be a per-page setting (Realtime/Analyzer
    // sections); it is now the single [Session] state. Seed it from the old
    // page keys so an existing user's TDM choice survives the upgrade.
    if (!cfg.value(QStringLiteral("Session")).contains(QStringLiteral("tdm_enabled"))) {
        const ConfigSection realtime = cfg.value(QStringLiteral("Realtime"));
        const ConfigSection analyzer = cfg.value(QStringLiteral("Analyzer"));
        if (realtime.contains(QStringLiteral("tdm_enabled")) ||
            analyzer.contains(QStringLiteral("tdm_enabled"))) {
            const bool oldEnabled =
                realtime.value(QStringLiteral("tdm_enabled"), QStringLiteral("0")).toInt() != 0 ||
                analyzer.value(QStringLiteral("tdm_enabled"), QStringLiteral("0")).toInt() != 0;
            const QString oldPhase =
                realtime.contains(QStringLiteral("tdm_phase"))
                    ? realtime.value(QStringLiteral("tdm_phase"))
                    : analyzer.value(QStringLiteral("tdm_phase"), QStringLiteral("0"));
            ConfigSection &session = cfg[QStringLiteral("Session")];
            session[QStringLiteral("tdm_enabled")] =
                oldEnabled ? QStringLiteral("1") : QStringLiteral("0");
            session[QStringLiteral("tdm_phase")] =
                oldPhase == QStringLiteral("1") ? QStringLiteral("1") : QStringLiteral("0");
            changed = true;
        }
    }

    for (auto secIt = def.cbegin(); secIt != def.cend(); ++secIt) {
        if (!cfg.contains(secIt.key())) {
            cfg[secIt.key()] = secIt.value();
            changed = true;
            continue;
        }
        for (auto kvIt = secIt.value().cbegin(); kvIt != secIt.value().cend(); ++kvIt) {
            if (!cfg[secIt.key()].contains(kvIt.key())) {
                cfg[secIt.key()][kvIt.key()] = kvIt.value();
                changed = true;
            }
        }
    }
    if (changed || migrateLegacy) {
        save(cfg);
    }
    return cfg;
}

}  // namespace ccv2
