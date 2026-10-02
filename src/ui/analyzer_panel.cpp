#include "ui/analyzer_panel.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QColor>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QMessageBox>
#include <QMetaObject>
#include <QNetworkProxy>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <QTcpSocket>
#include <QtConcurrent>
#include <stdexcept>

#include "core/constants.h"
#include "core/channel_routing.h"
#include "core/tdm_context.h"
#include "service/session_hub.h"
#include "signal/sndr_calculator.h"
#include "ui/waveform_widget.h"

namespace ccv2 {

namespace {

constexpr int kMaxPlottedChannels = 2;
constexpr int kAllChannelCount = 256;
constexpr double kAllChannelRefreshHz = 0.5;
constexpr int kAllChannelWavePoints = 1024;
constexpr int kAllChannelRenderPoints = 512;

struct FftStats {
    double fin{0.0};
    double sndr{0.0};
    double enob{0.0};
    double snr{0.0};
    double sfdr{0.0};
    double thd{0.0};
    double thdOdd{0.0};
    double thdEven{0.0};
    double irnPower{0.0};
    double irn{0.0};
    double fom{0.0};
};

struct FftStatsRow {
    const char *name;
    const char *key;
    const char *unit;
    const char *bestMode;
};

constexpr FftStatsRow kStatsRows[] = {
    {"Fin", "fin", "Hz", "none"},
    {"SNDR", "sndr", "dB", "max"},
    {"ENOB", "enob", "bit", "max"},
    {"SNR", "snr", "dB", "max"},
    {"SFDR", "sfdr", "dB", "max"},
    {"THD", "thd", "dB", "max"},
    {"THD_odd", "thdOdd", "dB", "max"},
    {"THD_even", "thdEven", "dB", "max"},
    {"IRN_Power", "irnPower", "dB", "min"},
    {"IRN", "irn", "uVrms", "min"},
    {"FoM", "fom", "dB", "max"},
};

struct FftInput {
    int ch;
    QVector<double> data;
    QColor color;
    QString label;
    bool statsSource{false};
};

struct ChannelMomentStats {
    double mean{0.0};
    double rms{0.0};
    double p2p{0.0};
    double minV{0.0};
    double maxV{0.0};
    int samples{0};
};

struct MultiChannelStats {
    int outputs{0};
    int samplesMin{0};
    int samplesMax{0};
    double meanAvg{0.0};
    double rmsAvg{0.0};
    double rmsMax{0.0};
    double p2pAvg{0.0};
    double p2pMax{0.0};
    double minV{0.0};
    double maxV{0.0};
};

struct TdmSeries {
    QVector<double> data;
    QVector<qint64> indices;
    int localEle{-1};
    int globalEle{-1};
    QString label;
};

int floorPowerOfTwo(int value) {
    int p = 1;
    while (p <= value / 2) {
        p *= 2;
    }
    return qMax(256, p);
}

void setGridLabelText(QGridLayout *layout, int row, int column, const QString &text) {
    QLayoutItem *item = layout->itemAtPosition(row, column);
    if (!item || !item->widget()) {
        return;
    }
    if (auto *label = qobject_cast<QLabel *>(item->widget())) {
        label->setText(text);
    }
}

void setStatusLine(QLabel *label, const QString &text, const char *state = "info")
{
    if (!label) {
        return;
    }
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void setBadgeLine(QLabel *label, const QString &text, const char *state = "info")
{
    if (!label) {
        return;
    }
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QString formatStatValue(const QString &key, double value, const QString &unit) {
    if (!std::isfinite(value)) {
        return QStringLiteral("-");
    }

    int decimals = 2;
    if (key == QStringLiteral("irn")) {
        decimals = 3;
    } else if (key == QStringLiteral("fin")) {
        decimals = 2;
    }

    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(unit);
}

ChannelMomentStats calculateMomentStats(const QVector<double> &data)
{
    ChannelMomentStats stats;
    stats.samples = data.size();
    if (data.isEmpty()) {
        return stats;
    }

    double sum = 0.0;
    double sumSq = 0.0;
    double minV = data.first();
    double maxV = data.first();
    for (double v : data) {
        sum += v;
        sumSq += v * v;
        minV = qMin(minV, v);
        maxV = qMax(maxV, v);
    }
    const double invN = 1.0 / static_cast<double>(data.size());
    stats.mean = sum * invN;
    const double variance = qMax(0.0, sumSq * invN - stats.mean * stats.mean);
    stats.rms = std::sqrt(variance);
    stats.p2p = maxV - minV;
    stats.minV = minV;
    stats.maxV = maxV;
    return stats;
}

MultiChannelStats summarizeMultiChannelStats(const QVector<WaveformWidget::PlotSeries> &series)
{
    MultiChannelStats summary;
    summary.minV = std::numeric_limits<double>::infinity();
    summary.maxV = -std::numeric_limits<double>::infinity();
    summary.samplesMin = std::numeric_limits<int>::max();

    for (const auto &s : series) {
        const ChannelMomentStats st = calculateMomentStats(s.data);
        if (st.samples <= 0) {
            continue;
        }
        ++summary.outputs;
        summary.samplesMin = qMin(summary.samplesMin, st.samples);
        summary.samplesMax = qMax(summary.samplesMax, st.samples);
        summary.meanAvg += st.mean;
        summary.rmsAvg += st.rms;
        summary.rmsMax = qMax(summary.rmsMax, st.rms);
        summary.p2pAvg += st.p2p;
        summary.p2pMax = qMax(summary.p2pMax, st.p2p);
        summary.minV = qMin(summary.minV, st.minV);
        summary.maxV = qMax(summary.maxV, st.maxV);
    }

    if (summary.outputs > 0) {
        const double invOutputs = 1.0 / static_cast<double>(summary.outputs);
        summary.meanAvg *= invOutputs;
        summary.rmsAvg *= invOutputs;
        summary.p2pAvg *= invOutputs;
    } else {
        summary.samplesMin = 0;
        summary.minV = 0.0;
        summary.maxV = 0.0;
    }
    return summary;
}

double statValue(const FftStats &stats, const QString &key) {
    if (key == QStringLiteral("fin")) return stats.fin;
    if (key == QStringLiteral("sndr")) return stats.sndr;
    if (key == QStringLiteral("enob")) return stats.enob;
    if (key == QStringLiteral("snr")) return stats.snr;
    if (key == QStringLiteral("sfdr")) return stats.sfdr;
    if (key == QStringLiteral("thd")) return stats.thd;
    if (key == QStringLiteral("thdOdd")) return stats.thdOdd;
    if (key == QStringLiteral("thdEven")) return stats.thdEven;
    if (key == QStringLiteral("irnPower")) return stats.irnPower;
    if (key == QStringLiteral("irn")) return stats.irn;
    if (key == QStringLiteral("fom")) return stats.fom;
    return 0.0;
}

QVector<double> convertAdcValues(const QVector<qint32> &raw) {
    QVector<double> out;
    out.reserve(raw.size());
    for (qint32 v : raw) {
        out.push_back(static_cast<double>(v) / static_cast<double>(1 << kAdcBits) * kVref);
    }
    return out;
}

QVector<TdmSeries> splitTdmSeries(int globalChannel,
                                  const QVector<qint64> &indices,
                                  const QVector<qint32> &raw,
                                  bool pair02)
{
    const QPair<int, int> pair = tdmLocalElePair(pair02);
    TdmSeries first;
    first.localEle = pair.first;
    first.globalEle = globalEleForChannelLocalEle(globalChannel, first.localEle);
    first.label = QStringLiteral("CH%1 L%2 ELE%3").arg(globalChannel).arg(first.localEle).arg(first.globalEle);

    TdmSeries second;
    second.localEle = pair.second;
    second.globalEle = globalEleForChannelLocalEle(globalChannel, second.localEle);
    second.label = QStringLiteral("CH%1 L%2 ELE%3").arg(globalChannel).arg(second.localEle).arg(second.globalEle);

    const int n = qMin(indices.size(), raw.size());
    first.data.reserve((n + kTdmPhaseCount - 1) / kTdmPhaseCount);
    second.data.reserve((n + kTdmPhaseCount - 1) / kTdmPhaseCount);
    for (int i = 0; i < n; ++i) {
        const int phase = tdmPhaseForFrame(indices[i]);
        const double value = static_cast<double>(raw[i]) / static_cast<double>(1 << kAdcBits) * kVref;
        if (phase == pair.first) {
            first.data.push_back(value);
            first.indices.push_back(indices[i]);
        } else if (phase == pair.second) {
            second.data.push_back(value);
            second.indices.push_back(indices[i]);
        }
    }
    return {first, second};
}

}  // namespace

class FftStatsTable : public QWidget {
public:
    explicit FftStatsTable(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto *title = new QLabel(QStringLiteral("FFT 性能统计"), this);
        title->setObjectName(QStringLiteral("fftStatsTitle"));
        title->setAlignment(Qt::AlignCenter);
        layout->addWidget(title);
        m_sourceLabel = new QLabel(QStringLiteral("Source: -"), this);
        m_sourceLabel->setProperty("role", "caption");
        m_sourceLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_sourceLabel);

        m_table = new QTableWidget(static_cast<int>(std::size(kStatsRows)), 3, this);
        m_table->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("当前值"), QStringLiteral("最优值")});
        m_table->verticalHeader()->setVisible(false);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionMode(QAbstractItemView::NoSelection);
        m_table->setFocusPolicy(Qt::NoFocus);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_table->horizontalHeader()->setMinimumHeight(24);
        m_table->verticalHeader()->setDefaultSectionSize(22);
        m_table->setMinimumHeight(270);
        m_table->setStyleSheet(QStringLiteral("QTableWidget::item { padding:3px; }"));

        for (int row = 0; row < m_table->rowCount(); ++row) {
            auto *name = new QTableWidgetItem(QString::fromLatin1(kStatsRows[row].name));
            name->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, 0, name);
            auto *current = new QTableWidgetItem(QStringLiteral("-"));
            current->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, 1, current);
            auto *best = new QTableWidgetItem(QStringLiteral("-"));
            best->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, 2, best);
        }
        layout->addWidget(m_table, 1);

        auto *resetButton = new QPushButton(QStringLiteral("重置最优值"), this);
        resetButton->setProperty("variant", "secondary");
        connect(resetButton, &QPushButton::clicked, this, &FftStatsTable::resetBest);
        layout->addWidget(resetButton);

        resetBest();
    }

    void resetBest() {
        m_best.clear();
        for (const FftStatsRow &row : kStatsRows) {
            const QString key = QString::fromLatin1(row.key);
            const QString mode = QString::fromLatin1(row.bestMode);
            if (mode == QStringLiteral("max")) {
                m_best.insert(key, -std::numeric_limits<double>::infinity());
            } else if (mode == QStringLiteral("min")) {
                m_best.insert(key, std::numeric_limits<double>::infinity());
            } else {
                m_best.insert(key, std::numeric_limits<double>::quiet_NaN());
            }
        }
        for (int row = 0; row < m_table->rowCount(); ++row) {
            m_table->item(row, 1)->setText(QStringLiteral("-"));
            m_table->item(row, 2)->setText(QStringLiteral("-"));
        }
    }

    void updateStats(const FftStats &stats) {
        m_analysisError.clear();
        setSourceLabel(m_source);
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const QString key = QString::fromLatin1(kStatsRows[row].key);
            const QString unit = QString::fromLatin1(kStatsRows[row].unit);
            const QString mode = QString::fromLatin1(kStatsRows[row].bestMode);
            const double value = statValue(stats, key);
            m_table->item(row, 1)->setText(formatStatValue(key, value, unit));

            if (mode == QStringLiteral("max")) {
                if (value > m_best.value(key)) {
                    m_best.insert(key, value);
                }
            } else if (mode == QStringLiteral("min")) {
                if (value < m_best.value(key)) {
                    m_best.insert(key, value);
                }
            } else {
                m_best.insert(key, value);
            }
            m_table->item(row, 2)->setText(formatStatValue(key, m_best.value(key), unit));
        }
    }

    void setSourceLabel(const QString &label) {
        m_source = label;
        if (m_sourceLabel) {
            m_sourceLabel->setText(label.isEmpty()
                                       ? QStringLiteral("Source: -")
                                       : QStringLiteral("Source: %1%2").arg(label, m_analysisError.isEmpty()
                                             ? QString() : QStringLiteral(" · 当前指标无效")));
            m_sourceLabel->setToolTip(m_analysisError);
        }
    }

    void invalidateCurrent(const QString &reason) {
        m_analysisError = reason;
        for (int row = 0; row < m_table->rowCount(); ++row) m_table->item(row, 1)->setText(QStringLiteral("-"));
        setSourceLabel(m_source);
    }

private:
    QString m_source;
    QString m_analysisError;
    QTableWidget *m_table{nullptr};
    QLabel *m_sourceLabel{nullptr};
    QMap<QString, double> m_best;
};

class MultiChannelStatsTable : public QWidget {
public:
    explicit MultiChannelStatsTable(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto *title = new QLabel(QStringLiteral("多通道时域统计"), this);
        title->setObjectName(QStringLiteral("fftStatsTitle"));
        title->setAlignment(Qt::AlignCenter);
        layout->addWidget(title);

        m_table = new QTableWidget(7, 2, this);
        m_table->setHorizontalHeaderLabels({QStringLiteral("参数"), QStringLiteral("当前值")});
        m_table->verticalHeader()->setVisible(false);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionMode(QAbstractItemView::NoSelection);
        m_table->setFocusPolicy(Qt::NoFocus);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_table->horizontalHeader()->setMinimumHeight(26);
        m_table->verticalHeader()->setDefaultSectionSize(28);
        m_table->setMinimumHeight(250);
        m_table->setStyleSheet(QStringLiteral("QTableWidget::item { padding:4px; }"));

        const QStringList names = {
            QStringLiteral("输出路数"),
            QStringLiteral("Samples"),
            QStringLiteral("Mean(avg)"),
            QStringLiteral("AC RMS(avg)"),
            QStringLiteral("AC RMS(max)"),
            QStringLiteral("p2p(avg)"),
            QStringLiteral("p2p(max)"),
        };
        for (int row = 0; row < names.size(); ++row) {
            auto *name = new QTableWidgetItem(names[row]);
            name->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, 0, name);
            auto *value = new QTableWidgetItem(QStringLiteral("-"));
            value->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, 1, value);
        }
        layout->addWidget(m_table, 1);

        auto *hint = new QLabel(QStringLiteral("AC RMS = sqrt(mean(x^2)-mean(x)^2)，已去除直流均值"), this);
        hint->setProperty("role", "caption");
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }

    void updateStats(const MultiChannelStats &stats) {
        if (stats.outputs <= 0) {
            reset();
            return;
        }
        m_table->item(0, 1)->setText(QString::number(stats.outputs));
        m_table->item(1, 1)->setText(QStringLiteral("%1..%2").arg(stats.samplesMin).arg(stats.samplesMax));
        m_table->item(2, 1)->setText(voltageText(stats.meanAvg));
        m_table->item(3, 1)->setText(voltageText(stats.rmsAvg));
        m_table->item(4, 1)->setText(voltageText(stats.rmsMax));
        m_table->item(5, 1)->setText(voltageText(stats.p2pAvg));
        m_table->item(6, 1)->setText(voltageText(stats.p2pMax));
    }

    void reset() {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            m_table->item(row, 1)->setText(QStringLiteral("-"));
        }
    }

private:
    QTableWidget *m_table{nullptr};
};

AnalyzerPanel::AnalyzerPanel(ConfigManager *cfgMgr,
                             NetworkState *networkState,
                             ChannelAddressState *channelState,
                             QWidget *parent)
    : QWidget(parent),
      m_cfgMgr(cfgMgr),
      m_networkState(networkState),
      m_channelState(channelState),
      m_rawQueue(std::make_shared<ThreadSafeQueue<QByteArray>>(200)),
      m_stopFlag(std::make_unique<std::atomic_bool>(false)) {
    m_saveDebounceTimer.setSingleShot(true);
    m_saveDebounceTimer.setInterval(500);
    connect(&m_saveDebounceTimer, &QTimer::timeout, this, &AnalyzerPanel::saveAnalyzerConfigNow);

    ConfigMap parser = cfgMgr->load();
    m_cfg.host = parser.value(QStringLiteral("Network")).value(QStringLiteral("host"), QStringLiteral("localhost"));
    m_cfg.port = parser.value(QStringLiteral("Network")).value(QStringLiteral("port"), QStringLiteral("10086")).toInt();
    m_cfg.samplingRate = parser.value(QStringLiteral("Signal")).value(QStringLiteral("sampling_rate"), QStringLiteral("20000")).toDouble();
    m_cfg.refreshHz = parser.value(QStringLiteral("Signal")).value(QStringLiteral("refresh_hz"), QStringLiteral("10")).toDouble();
    m_cfg.wavePoints = parser.value(QStringLiteral("Signal")).value(QStringLiteral("wave_points"), QStringLiteral("8000")).toInt();
    m_cfg.fftEnabled = parser.value(QStringLiteral("Signal")).value(QStringLiteral("fft_enabled"), QStringLiteral("1")).toInt() != 0;
    m_cfg.fftPoints = parser.value(QStringLiteral("Signal")).value(QStringLiteral("fft_points"), QStringLiteral("2048")).toInt();
    m_cfg.fftWindow = parser.value(QStringLiteral("Signal")).value(QStringLiteral("fft_window"), QStringLiteral("hann"));
    m_cfg.fftDcBins = parser.value(QStringLiteral("Signal")).value(QStringLiteral("fft_dc_bins"), QStringLiteral("5")).toInt();
    m_cfg.fftSigBins = parser.value(QStringLiteral("Signal")).value(QStringLiteral("fft_sig_bins"), QStringLiteral("5")).toInt();
    m_cfg.fftBandwidthHz = parser.value(QStringLiteral("Signal"))
                                .value(QStringLiteral("fft_bandwidth_hz"),
                                       QString::number(m_cfg.samplingRate / 2.0))
                                .toDouble();
    if (m_cfg.fftBandwidthHz <= 0.0) {
        m_cfg.fftBandwidthHz = m_cfg.samplingRate / 2.0;
    }
    m_cfg.irnGain = parser.value(QStringLiteral("Signal")).value(QStringLiteral("irn_gain"), QStringLiteral("60")).toDouble();
    if (m_cfg.irnGain <= 0.0) {
        m_cfg.irnGain = 60.0;
    }
    m_cfg.fftYMode = parser.value(QStringLiteral("Signal"))
                         .value(QStringLiteral("fft_y_mode"), QStringLiteral("psd_db"))
                         .trimmed()
                         .toLower();
    if (m_cfg.fftYMode != QStringLiteral("density")) {
        m_cfg.fftYMode = QStringLiteral("psd_db");
    }
    m_cfg.channels = parser.value(QStringLiteral("Analyzer")).value(QStringLiteral("channels"), QStringLiteral("239,240"));
    if (m_cfg.channels.trimmed() == QStringLiteral("0-255")) {
        m_cfg.channels = QStringLiteral("239,240");
        m_cfg.refreshHz = qMax(10.0, m_cfg.refreshHz);
        m_cfg.wavePoints = qMax(8000, m_cfg.wavePoints);
        m_cfg.fftEnabled = true;
        parser[QStringLiteral("Analyzer")][QStringLiteral("channels")] = m_cfg.channels;
        parser[QStringLiteral("Signal")][QStringLiteral("refresh_hz")] =
            QString::number(m_cfg.refreshHz, 'g', 12);
        parser[QStringLiteral("Signal")][QStringLiteral("wave_points")] =
            QString::number(m_cfg.wavePoints);
        parser[QStringLiteral("Signal")][QStringLiteral("fft_enabled")] =
            QStringLiteral("1");
        cfgMgr->save(parser);
    }
    // TDM state is global ([Session] via SessionHub's TdmContext); the
    // controls are synced from it in setSessionHub.
    m_cfg.statsSource = parser.value(QStringLiteral("Analyzer")).value(QStringLiteral("stats_source"), QString());
    m_cfg.pauseKeepCapture = parser.value(QStringLiteral("Analyzer")).value(QStringLiteral("pause_keep_capture"), QStringLiteral("1")).toInt() != 0;
    m_cfg.saveDir =
        parser.value(QStringLiteral("Recording"))
            .value(QStringLiteral("save_dir"),
                   parser.value(QStringLiteral("Paths"))
                       .value(QStringLiteral("save_dir"),
                              QStringLiteral("d:/ADC_data")));

    auto *main = new QHBoxLayout(this);
    main->setContentsMargins(8, 8, 8, 8);

    auto *control = new QScrollArea;
    control->setObjectName(QStringLiteral("analyzerControlPanel"));
    control->setWidgetResizable(true);
    control->setMinimumWidth(288);
    control->setMaximumWidth(350);
    control->setFrameShape(QFrame::NoFrame);
    control->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *controlBody = new QWidget;
    auto *controlLayout = new QVBoxLayout(controlBody);
    controlLayout->setContentsMargins(0, 0, 5, 0);
    controlLayout->setSpacing(6);

    m_netLabel = new QLabel(controlBody);
    m_netLabel->hide();
    m_netStateLabel = new QLabel(QStringLiteral("待检测"), controlBody);
    m_netStateLabel->setObjectName(QStringLiteral("networkStateBadge"));
    m_netStateLabel->hide();
    m_btnProbeNetwork = new QPushButton(QStringLiteral("检查连接"), controlBody);
    m_btnProbeNetwork->hide();

    auto *grpChannels = new QGroupBox(QStringLiteral("通道选择"));
    auto *channelForm = new QFormLayout(grpChannels);
    channelForm->setHorizontalSpacing(8);
    channelForm->setVerticalSpacing(5);
    m_channelsEdit = new QLineEdit(m_cfg.channels);
    m_channelsEdit->setToolTip(QStringLiteral("普通分析最多2个通道，例如 239,240；全通道概览使用 0-255"));
    m_modeStatusLabel = new QLabel;
    m_modeStatusLabel->setProperty("class", "status-badge");
    m_modeStatusLabel->setProperty("state", "ok");
    m_modeStatusLabel->setAlignment(Qt::AlignCenter);
    m_modeStatusLabel->setMinimumHeight(24);
    m_btnGetAddrAnalyzer = new QPushButton(QStringLiteral("从通道图载入"));
    m_btnGetAddrAnalyzer->setProperty("variant", "secondary");
    m_btnAllChannelsOverview = new QPushButton(QStringLiteral("256通道概览"));
    m_btnAllChannelsOverview->setCheckable(true);
    m_btnAllChannelsOverview->setProperty("variant", "secondary");
    m_btnAllChannelsOverview->setToolTip(QStringLiteral("载入0-255，强制0.5 Hz刷新、1024点窗口、关闭FFT"));
    m_tdmCheckbox = new QCheckBox(QStringLiteral("TDM分组显示"));
    m_tdmCheckbox->setChecked(false);
    m_tdmCheckbox->setToolTip(QStringLiteral(
        "全局 TDM 状态：与实时波形/实时Spike页联动，\n随录制写入 session.json，回放时自动恢复。"));
    m_tdmPhaseCombo = new QComboBox;
    m_tdmPhaseCombo->addItem(QStringLiteral("显示相位 0 / 2"), 0);
    m_tdmPhaseCombo->addItem(QStringLiteral("显示相位 1 / 3"), 1);
    m_tdmPhaseCombo->setEnabled(false);
    m_pauseKeepCaptureCheckbox = new QCheckBox(QStringLiteral("暂停时继续采集"));
    m_pauseKeepCaptureCheckbox->setChecked(m_cfg.pauseKeepCapture);
    m_statsSourceCombo = new QComboBox;
    m_statsSourceCombo->setToolTip(QStringLiteral("选择右侧 FFT 性能统计使用哪一路输出"));
    channelForm->addRow(QStringLiteral("模式"), m_modeStatusLabel);
    channelForm->addRow(QStringLiteral("通道"), m_channelsEdit);
    auto *channelButtons = new QHBoxLayout;
    channelButtons->addWidget(m_btnGetAddrAnalyzer);
    channelButtons->addWidget(m_btnAllChannelsOverview);
    channelForm->addRow(channelButtons);
    channelForm->addRow(m_tdmCheckbox);
    channelForm->addRow(QStringLiteral("TDM相位"), m_tdmPhaseCombo);
    channelForm->addRow(QStringLiteral("统计输出"), m_statsSourceCombo);
    channelForm->addRow(m_pauseKeepCaptureCheckbox);
    controlLayout->addWidget(grpChannels);

    auto *grpAcq = new QGroupBox(QStringLiteral("采集参数"));
    auto *acqForm = new QFormLayout(grpAcq);
    acqForm->setHorizontalSpacing(8);
    acqForm->setVerticalSpacing(5);
    m_refreshSpin = new QDoubleSpinBox;
    m_refreshSpin->setRange(kAllChannelRefreshHz, 60.0);
    m_refreshSpin->setDecimals(1);
    m_refreshSpin->setSingleStep(0.5);
    m_refreshSpin->setSuffix(QStringLiteral(" Hz"));
    m_refreshSpin->setValue(qMax(kAllChannelRefreshHz, m_cfg.refreshHz));
    m_fsSpin = new QDoubleSpinBox;
    m_fsSpin->setRange(1000.0, 1000000.0);
    m_fsSpin->setDecimals(2);
    m_fsSpin->setValue(m_cfg.samplingRate);
    m_cfg.samplingRate = m_fsSpin->value();
    m_wavePointsSpin = new QSpinBox;
    m_wavePointsSpin->setRange(256, 262144);
    m_wavePointsSpin->setSingleStep(256);
    m_wavePointsSpin->setValue(qMax(256, m_cfg.wavePoints));
    m_fftEnabledCheckbox = new QCheckBox(QStringLiteral("显示/计算 FFT"));
    m_fftEnabledCheckbox->setChecked(m_cfg.fftEnabled);
    m_fftPointsCombo = new QComboBox;
    for (int points = 256; points <= 262144; points *= 2) {
        m_fftPointsCombo->addItem(QString::number(points), points);
    }
    const int configuredFftPoints = floorPowerOfTwo(qMax(256, m_cfg.fftPoints));
    const int fftPointIndex = m_fftPointsCombo->findData(configuredFftPoints);
    m_fftPointsCombo->setCurrentIndex(qMax(0, fftPointIndex));
    m_fftWindowCombo = new QComboBox;
    m_fftWindowCombo->addItem(QStringLiteral("Rect"), QStringLiteral("rect"));
    m_fftWindowCombo->addItem(QStringLiteral("Hann"), QStringLiteral("hann"));
    m_fftWindowCombo->addItem(QStringLiteral("Blackman"), QStringLiteral("blackman"));
    m_fftWindowCombo->addItem(QStringLiteral("Blackman-Harris"), QStringLiteral("blackmanharris"));
    m_fftWindowCombo->addItem(QStringLiteral("Kaiser"), QStringLiteral("kaiser"));
    const int windowIndex = m_fftWindowCombo->findData(m_cfg.fftWindow.trimmed().toLower());
    m_fftWindowCombo->setCurrentIndex(windowIndex >= 0 ? windowIndex : 1);
    m_fftDcBinsSpin = new QSpinBox;
    m_fftDcBinsSpin->setRange(0, 2048);
    m_fftDcBinsSpin->setValue(qMax(0, m_cfg.fftDcBins));
    m_fftSigBinsSpin = new QSpinBox;
    m_fftSigBinsSpin->setRange(1, 2048);
    m_fftSigBinsSpin->setValue(qMax(1, m_cfg.fftSigBins));
    m_fftBandwidthSpin = new QDoubleSpinBox;
    m_fftBandwidthSpin->setRange(1.0, 10'000'000.0);
    m_fftBandwidthSpin->setDecimals(1);
    m_fftBandwidthSpin->setSingleStep(1000.0);
    m_fftBandwidthSpin->setSuffix(QStringLiteral(" Hz"));
    m_fftBandwidthSpin->setToolTip(QStringLiteral("SNDR/SNR/IRN 统计带宽；默认 fs/2，可手动改小"));
    m_fftBandwidthSpin->setValue(qMax(1.0, m_cfg.fftBandwidthHz));
    m_irnGainSpin = new QDoubleSpinBox;
    m_irnGainSpin->setRange(0.001, 1'000'000.0);
    m_irnGainSpin->setDecimals(3);
    m_irnGainSpin->setSingleStep(1.0);
    m_irnGainSpin->setSuffix(QStringLiteral(" x"));
    m_irnGainSpin->setToolTip(QStringLiteral("等效输入噪声 = 输出积分噪声 / Gain * 1e6 (uVrms)"));
    m_irnGainSpin->setValue(qMax(0.001, m_cfg.irnGain));
    m_fftYModeCombo = new QComboBox;
    m_fftYModeCombo->addItem(QStringLiteral("功率谱 dB"), QStringLiteral("psd_db"));
    m_fftYModeCombo->addItem(QStringLiteral("噪声密度 nV/√Hz"), QStringLiteral("density"));
    m_fftYModeCombo->setToolTip(QStringLiteral(
        "噪声密度 = sqrt(2·PSD)/IRN Gain，输入参考；Y 轴按整 decade 自动量程，上限 1 V/√Hz"));
    if (m_cfg.fftYMode == QStringLiteral("density")) {
        m_fftYModeCombo->setCurrentIndex(1);
    }
    acqForm->addRow(QStringLiteral("刷新率"), m_refreshSpin);
    acqForm->addRow(QStringLiteral("采样率 fs"), m_fsSpin);
    acqForm->addRow(QStringLiteral("波形点数"), m_wavePointsSpin);
    acqForm->addRow(m_fftEnabledCheckbox);
    acqForm->addRow(QStringLiteral("FFT 点数"), m_fftPointsCombo);
    acqForm->addRow(QStringLiteral("FFT 窗"), m_fftWindowCombo);
    acqForm->addRow(QStringLiteral("DC Bin"), m_fftDcBinsSpin);
    acqForm->addRow(QStringLiteral("Sig Bin ±"), m_fftSigBinsSpin);
    acqForm->addRow(QStringLiteral("SNDR BW"), m_fftBandwidthSpin);
    acqForm->addRow(QStringLiteral("IRN Gain"), m_irnGainSpin);
    acqForm->addRow(QStringLiteral("FFT 纵轴"), m_fftYModeCombo);
    controlLayout->addWidget(grpAcq);

    // Offline MATLAB export lives in the left 采集存储 panel (storage/IO home).

    auto *grpRun = new QGroupBox(QStringLiteral("会话状态"));
    auto *runLayout = new QVBoxLayout(grpRun);
    runLayout->setContentsMargins(8, 10, 8, 8);
    m_statusLabel = new QLabel(QStringLiteral("状态: 空闲"));
    m_statusLabel->setProperty("role", "status-line");
    m_statusLabel->setProperty("state", "info");
    m_statusLabel->setWordWrap(true);
    runLayout->addWidget(m_statusLabel);
    controlLayout->addWidget(grpRun);

    controlLayout->addStretch(1);
    control->setWidget(controlBody);

    auto *plots = new QWidget;
    plots->setProperty("role", "plot-area");
    auto *plotsLayout = new QGridLayout(plots);
    plotsLayout->setContentsMargins(6, 0, 6, 0);
    plotsLayout->setSpacing(8);
    plots->setMinimumWidth(620);
    m_wavePlot = new WaveformWidget(QStringLiteral("时域波形"));
    m_wavePlot->setYRange(0.0, 1.8);
    m_wavePlot->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("电压 (V)"));
    m_wavePlot->setMaxRenderPoints(0);
    m_wavePlot->setDenseGrid(true);
    m_wavePlot->setMinimumHeight(260);
    plotsLayout->addWidget(m_wavePlot, 0, 0);

    m_fftPlot = new WaveformWidget(QStringLiteral("频谱"));
    m_fftPlot->setLogXScale(true);
    m_fftPlot->setMaxRenderPoints(0);
    m_fftPlot->setDenseGrid(true);
    m_fftPlot->setMinimumHeight(260);
    applyFftYAxisMode();
    plotsLayout->addWidget(m_fftPlot, 1, 0);

    m_timeStatsPanel = new QWidget;
    m_timeStatsPanel->setProperty("role", "side-panel");
    m_timeStatsPanel->setMinimumWidth(350);
    m_timeStatsPanel->setMaximumWidth(380);
    auto *timeStatsLayout = new QVBoxLayout(m_timeStatsPanel);
    timeStatsLayout->setContentsMargins(6, 0, 0, 0);
    timeStatsLayout->setSpacing(8);
    m_timeStats = new MultiChannelStatsTable(m_timeStatsPanel);
    timeStatsLayout->addWidget(m_timeStats);
    timeStatsLayout->addStretch(1);
    plotsLayout->addWidget(m_timeStatsPanel, 0, 1);

    m_fftStatsPanel = new QWidget;
    m_fftStatsPanel->setProperty("role", "side-panel");
    m_fftStatsPanel->setMinimumWidth(350);
    m_fftStatsPanel->setMaximumWidth(380);
    auto *statsLayout = new QVBoxLayout(m_fftStatsPanel);
    statsLayout->setContentsMargins(6, 0, 0, 0);
    statsLayout->setSpacing(8);
    m_fftStats = new FftStatsTable(m_fftStatsPanel);
    statsLayout->addWidget(m_fftStats);
    statsLayout->addStretch(1);
    plotsLayout->addWidget(m_fftStatsPanel, 1, 1);
    plotsLayout->setColumnStretch(0, 1);
    plotsLayout->setColumnStretch(1, 0);
    plotsLayout->setRowStretch(0, 1);
    plotsLayout->setRowStretch(1, 1);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(control);
    splitter->addWidget(plots);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({340, 1400});
    main->addWidget(splitter, 1);

    connect(&m_timer, &QTimer::timeout, this, &AnalyzerPanel::updatePlots);
    connect(m_btnGetAddrAnalyzer, &QPushButton::clicked, this, &AnalyzerPanel::onGetChannelAddress);
    connect(m_btnAllChannelsOverview, &QPushButton::toggled, this, &AnalyzerPanel::setAllChannelsOverviewMode);
    connect(m_btnProbeNetwork, &QPushButton::clicked, this, &AnalyzerPanel::checkNetworkConnectivity);
    connect(m_channelsEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_allChannelsOverviewMode && text.trimmed() != QStringLiteral("0-255")) {
            const QString requestedChannels = text.trimmed();
            setAllChannelsOverviewMode(false);
            if (m_channelsEdit && !requestedChannels.isEmpty()) {
                const QSignalBlocker blocker(m_channelsEdit);
                m_channelsEdit->setText(requestedChannels);
            }
            setStatusLine(m_statusLabel,
                          QStringLiteral("状态: 已切回普通分析，FFT 控件已恢复可选"),
                          "ok");
        }
        scheduleSaveAnalyzerConfig();
    });
    connect(m_channelsEdit, &QLineEdit::editingFinished, this, [this]() {
        restartPipelineIfRunning();
    });
    connect(m_refreshSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        scheduleSaveAnalyzerConfig();
        if (m_timer.isActive()) {
            startTimer();
        }
    });
    connect(m_statsSourceCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
    });
    // The widgets mirror the global TdmContext; applyGlobalTdmState (driven
    // by the context's changed signal) performs the actual page update, also
    // when the change came from another page or a replay restore. Overview
    // mode never reaches here: it flips the widgets under QSignalBlocker.
    connect(m_tdmCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_hub && m_hub->tdmContext() && !m_allChannelsOverviewMode) {
            m_hub->tdmContext()->setEnabled(checked);
            return;
        }
        if (m_tdmPhaseCombo) {
            m_tdmPhaseCombo->setEnabled(checked);
        }
        resetFftStats();
    });
    connect(m_tdmPhaseCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_hub && m_hub->tdmContext() && !m_allChannelsOverviewMode) {
            m_hub->tdmContext()->setPair02(
                m_tdmPhaseCombo->currentData().toInt() == 0);
            return;
        }
        resetFftStats();
    });
    connect(m_pauseKeepCaptureCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_isPaused) {
            m_processEnabledAtomic.store(checked);
        }
        scheduleSaveAnalyzerConfig();
    });
    connect(m_fftEnabledCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_allChannelsOverviewMode && checked) {
            const QSignalBlocker blocker(m_fftEnabledCheckbox);
            m_fftEnabledCheckbox->setChecked(false);
            checked = false;
        }
        syncFftUiState();
        if (checked) {
            resetFftStats();
        }
        scheduleSaveAnalyzerConfig();
        if (m_statusLabel) {
            setStatusLine(m_statusLabel,
                          checked ? QStringLiteral("状态: FFT 已开启")
                                  : QStringLiteral("状态: FFT 已关闭，仅刷新时域波形"),
                          checked ? "ok" : "warn");
        }
    });
    connect(m_fsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double fs) {
        if (!m_hub || !m_hub->isReplaying()) m_cfg.samplingRate = fs;
        resetFftStats();
        const bool tdm = m_tdmCheckbox && m_tdmCheckbox->isChecked();
        const double nyquist = fs / (tdm ? (2.0 * kTdmPhaseCount) : 2.0);
        if (m_fftBandwidthSpin) {
            const QSignalBlocker blocker(m_fftBandwidthSpin);
            m_fftBandwidthSpin->setMaximum(nyquist);
            if (m_fftBandwidthSpin->value() > nyquist) {
                m_fftBandwidthSpin->setValue(nyquist);
            }
        }
        scheduleSaveAnalyzerConfig();
    });
    connect(m_wavePointsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        scheduleSaveAnalyzerConfig();
    });
    connect(m_wavePointsSpin, &QSpinBox::editingFinished, this, [this]() {
        restartPipelineIfRunning();
    });
    connect(m_fftPointsCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
        restartPipelineIfRunning();
    });
    connect(m_fftWindowCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
    });
    connect(m_fftDcBinsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
    });
    connect(m_fftSigBinsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
    });
    connect(m_fftBandwidthSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
    });
    connect(m_irnGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        resetFftStats();
        scheduleSaveAnalyzerConfig();
    });
    connect(m_fftYModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        applyFftYAxisMode();
        m_fftPlot->setSeries({});  // old-mode curves are in the wrong unit
        // Invalidate in-flight jobs without resetting the stats table: the
        // stats are display-mode independent, only the curves change unit.
        m_analysisGeneration.fetch_add(1);
        scheduleSaveAnalyzerConfig();
    });
    connect(m_networkState, &NetworkState::endpointChanged, this, &AnalyzerPanel::onNetworkChanged);
    connect(m_networkState, &NetworkState::dataPortChanged, this, [this](int) {
        onNetworkChanged(m_networkState->host(), m_networkState->port());
    });

    onNetworkChanged(m_networkState->host(), m_networkState->port());
    syncFftUiState();
}

QPair<QVector<QVector<int>>, QVector<int>> AnalyzerPanel::parseChannelGroups(const QString &text) const {
    QVector<QVector<int>> groups;
    QVector<int> flat;
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        return {QVector<QVector<int>>{QVector<int>{243}}, QVector<int>{243}};
    }

    const QStringList groupTexts = t.split(';', Qt::SkipEmptyParts);
    QSet<int> seenGlobal;
    for (const QString &grp : groupTexts) {
        const QStringList items = grp.split(',', Qt::SkipEmptyParts);
        QVector<int> g;
        for (const QString &it : items) {
            const QString token = it.trimmed();
            if (token.isEmpty()) {
                continue;
            }

            QVector<int> parsed;
            if (token.contains('-')) {
                const QStringList ends = token.split('-', Qt::KeepEmptyParts);
                if (ends.size() != 2) {
                    throw std::runtime_error("invalid channel range");
                }
                bool okA = false;
                bool okB = false;
                int a = ends[0].trimmed().toInt(&okA);
                int b = ends[1].trimmed().toInt(&okB);
                if (!okA || !okB) {
                    throw std::runtime_error("invalid channel range");
                }
                if (a > b) {
                    std::swap(a, b);
                }
                if (a < 0 || b >= kChannelsTotal) {
                    throw std::runtime_error("channel out of range");
                }
                parsed.reserve(b - a + 1);
                for (int ch = a; ch <= b; ++ch) {
                    parsed.push_back(ch);
                }
            } else {
                bool ok = false;
                const int ch = token.toInt(&ok);
                if (!ok || ch < 0 || ch >= kChannelsTotal) {
                    throw std::runtime_error("channel out of range");
                }
                parsed.push_back(ch);
            }

            for (int ch : parsed) {
                if (seenGlobal.contains(ch)) {
                    throw std::runtime_error("duplicate channel");
                }
                seenGlobal.insert(ch);
                g.push_back(ch);
                flat.push_back(ch);
            }
        }
        if (!g.isEmpty()) {
            groups.push_back(g);
        }
    }

    if (groups.isEmpty()) {
        throw std::runtime_error("no valid channel");
    }
    if (flat.size() > maxSelectableChannels()) {
        throw std::runtime_error(m_allChannelsOverviewMode
                                     ? "256通道概览最多选择256个通道"
                                     : "普通分析最多只能选择2个通道");
    }

    QVector<int> unique;
    QSet<int> seen;
    for (int ch : flat) {
        if (!seen.contains(ch)) {
            seen.insert(ch);
            unique.push_back(ch);
        }
    }
    return {groups, unique};
}

QPair<QVector<QVector<int>>, QVector<int>> AnalyzerPanel::getChannels() const {
    return parseChannelGroups(m_channelsEdit->text());
}

QPair<QVector<QVector<int>>, QVector<int>> AnalyzerPanel::initBuffersAndCurves() {
    auto gv = getChannels();
    m_groups = gv.first;
    m_channels = gv.second;

    const int maxPoints = m_allChannelsOverviewMode
                              ? kAllChannelWavePoints
                              : qMax(m_wavePointsSpin->value(), fftPointCount());
    const int capacity = m_allChannelsOverviewMode
                             ? qMax(maxPoints * 4, 4096)
                             : qMax(maxPoints * 8, 65536);
    m_dataStore.reset(m_channels, capacity);

    return gv;
}

void AnalyzerPanel::setupThreads(const QVector<int> &channels) {
    m_stopFlag = std::make_unique<std::atomic_bool>(false);
    m_rawQueue->clear();
    m_rxBytes = 0;
    m_parsedFrames = 0;
    m_droppedFrames = 0;
    m_leftoverBytes = 0;
    m_lastBufferSamples = 0;

    m_processEnabledAtomic.store(true);

    qint64 initialFrameIndex = 0;
    if (m_hub) {
        initialFrameIndex = m_hub->addSubscriberWithFrameOrigin(m_rawQueue);
    }
    const int maxPoints = m_allChannelsOverviewMode
                              ? kAllChannelWavePoints
                              : qMax(m_wavePointsSpin->value(), fftPointCount());
    const int capacity = m_allChannelsOverviewMode
                             ? qMax(maxPoints * 4, 4096)
                             : qMax(maxPoints * 8, 65536);
    const quint64 epoch = m_hub ? m_hub->timelineEpoch() : 0;
    m_timelineEpoch.store(epoch);
    m_dataStore.reset(channels, capacity, initialFrameIndex, epoch);

    m_sorter = new SorterWorker(
        m_rawQueue,
        &m_dataStore,
        channels,
        nullptr,
        []() { return false; },
        [this]() { return m_processEnabledAtomic.load(); },
        m_stopFlag.get(),
        m_hub ? m_hub->timelineEpochCounter() : nullptr,
        this);
    SorterWorker *const activeSorter = m_sorter;
    m_firstRefreshPending = true;
    connect(m_sorter, &SorterWorker::framesParsed, this,
            [this, activeSorter](int frameCount, int leftoverBytes) {
        if (m_sorter != activeSorter) return;
        m_parsedFrames += frameCount;
        m_leftoverBytes = leftoverBytes;
        if (m_firstRefreshPending && m_isRunning) {
            m_firstRefreshPending = false;
            QTimer::singleShot(0, this, &AnalyzerPanel::updatePlots);
        }
    });
    m_sorter->start();
}

void AnalyzerPanel::onActivated() {
    if (m_hub && m_hub->isRunning() && !m_isRunning) {
        startLive();
    }
}

void AnalyzerPanel::onDeactivated() {
    if (m_isRunning) {
        stopAll();
    }
}

void AnalyzerPanel::startLive() {
    if (m_isRunning) {
        setStatusLine(m_statusLabel, QStringLiteral("状态: 已在运行"), "warn");
        return;
    }

    // A previous run's sorter may still be draining. stopAll()'s 600+200ms
    // waits cover the sorter's 500ms queue-pop timeout in the common case;
    // this guard is the backstop for the residual one (sorter mid-chunk or
    // starved by the scheduler): its stop flag is already set, so join it
    // here before m_stopFlag / m_dataStore / m_rawQueue are rebuilt
    // underneath it (realtime/sweep pages carry the same guard).
    if (m_sorter) {
        if (m_rawQueue) m_rawQueue->wakeAll();
        if (m_sorter->isRunning() && !m_sorter->wait(1500)) {
            setStatusLine(m_statusLabel,
                          QStringLiteral("错误: 上一轮后台线程尚未退出，请稍后重试"),
                          "error");
            return;
        }
        m_sorter->deleteLater();
        m_sorter = nullptr;
    }

    if (m_hub && m_fsSpin) {
        const double sessionSampleRate = m_hub->sampleRate();
        const QSignalBlocker blocker(m_fsSpin);
        m_fsSpin->setValue(sessionSampleRate);
        if (m_fftBandwidthSpin) {
            const bool tdm = m_tdmCheckbox && m_tdmCheckbox->isChecked();
            m_fftBandwidthSpin->setMaximum(
                sessionSampleRate / (tdm ? (2.0 * kTdmPhaseCount) : 2.0));
        }
        m_fsSpin->setEnabled(false);
    }
    scheduleSaveAnalyzerConfig();
    try {
        auto gv = initBuffersAndCurves();
        resetFftStats();
        setupThreads(gv.second);
    } catch (const std::exception &exc) {
        if (m_fsSpin) m_fsSpin->setEnabled(true);
        setStatusLine(m_statusLabel, QStringLiteral("状态: 通道参数错误: ") + QString::fromUtf8(exc.what()), "error");
        return;
    }

    startTimer();
    m_isRunning = true;
    m_isPaused = false;
    setStatusLine(m_statusLabel, QStringLiteral("状态: 实时采集中"), "ok");
}

void AnalyzerPanel::startTimer() {
    const double refreshHz = m_refreshSpin ? qMax(0.1, m_refreshSpin->value()) : 10.0;
    const int intervalMs = qMax(16, static_cast<int>(std::round(1000.0 / refreshHz)));
    m_timer.start(intervalMs);
}

void AnalyzerPanel::resetFftStats() {
    // Every semantic analysis change (TDM mode/phase, stats source, fs, FFT
    // points/window/bins, pipeline rebuild, timeline reset) funnels through
    // here. Bumping the generation invalidates in-flight FFT jobs so a result
    // computed under the old settings can never land in the new ones — in
    // particular it can no longer seed the "best value" column it just reset.
    m_analysisGeneration.fetch_add(1);
    if (m_fftStats) {
        m_fftStats->resetBest();
    }
}

void AnalyzerPanel::scheduleSaveAnalyzerConfig()
{
    if (!m_cfgMgr || m_shuttingDown.load()) {
        return;
    }
    m_saveDebounceTimer.start();
}

void AnalyzerPanel::saveAnalyzerConfigNow() const {
    if (!m_cfgMgr) {
        return;
    }

    ConfigMap cfg = m_cfgMgr->load();
    ConfigSection &signal = cfg[QStringLiteral("Signal")];
    signal[QStringLiteral("sampling_rate")] = QString::number(m_cfg.samplingRate, 'g', 12);
    const double refreshHz = m_allChannelsOverviewMode
                                 ? m_overviewSavedRefreshHz
                                 : (m_refreshSpin ? m_refreshSpin->value() : m_cfg.refreshHz);
    const int wavePoints = m_allChannelsOverviewMode
                               ? m_overviewSavedWavePoints
                               : (m_wavePointsSpin ? m_wavePointsSpin->value() : m_cfg.wavePoints);
    const bool fftEnabled = m_allChannelsOverviewMode
                                ? m_overviewSavedFftEnabled
                                : (m_fftEnabledCheckbox && m_fftEnabledCheckbox->isChecked());
    signal[QStringLiteral("refresh_hz")] = QString::number(refreshHz, 'g', 12);
    signal[QStringLiteral("wave_points")] = QString::number(wavePoints);
    signal[QStringLiteral("fft_enabled")] = fftEnabled ? QStringLiteral("1") : QStringLiteral("0");
    signal[QStringLiteral("fft_points")] = QString::number(fftPointCount());
    signal[QStringLiteral("fft_window")] =
        m_fftWindowCombo ? m_fftWindowCombo->currentData().toString() : m_cfg.fftWindow;
    signal[QStringLiteral("fft_dc_bins")] = QString::number(m_fftDcBinsSpin ? m_fftDcBinsSpin->value() : m_cfg.fftDcBins);
    signal[QStringLiteral("fft_sig_bins")] = QString::number(m_fftSigBinsSpin ? m_fftSigBinsSpin->value() : m_cfg.fftSigBins);
    signal[QStringLiteral("fft_bandwidth_hz")] =
        QString::number(m_fftBandwidthSpin ? m_fftBandwidthSpin->value() : m_cfg.fftBandwidthHz, 'g', 12);
    signal[QStringLiteral("irn_gain")] =
        QString::number(m_irnGainSpin ? m_irnGainSpin->value() : m_cfg.irnGain, 'g', 12);
    signal[QStringLiteral("fft_y_mode")] =
        m_fftYModeCombo ? m_fftYModeCombo->currentData().toString() : m_cfg.fftYMode;

    ConfigSection &analyzer = cfg[QStringLiteral("Analyzer")];
    analyzer[QStringLiteral("channels")] =
        m_allChannelsOverviewMode
            ? m_overviewSavedChannels
            : (m_channelsEdit ? m_channelsEdit->text().trimmed() : m_cfg.channels);
    // TDM moved to the global [Session] section; drop the stale page keys.
    analyzer.remove(QStringLiteral("tdm_enabled"));
    analyzer.remove(QStringLiteral("tdm_phase"));
    analyzer[QStringLiteral("stats_source")] = m_statsSourceCombo ? m_statsSourceCombo->currentText() : m_cfg.statsSource;
    analyzer[QStringLiteral("pause_keep_capture")] =
        (m_pauseKeepCaptureCheckbox && m_pauseKeepCaptureCheckbox->isChecked()) ? QStringLiteral("1") : QStringLiteral("0");
    m_cfgMgr->save(cfg);
}

bool AnalyzerPanel::fftDensityMode() const
{
    return m_fftYModeCombo
           && m_fftYModeCombo->currentData().toString() == QStringLiteral("density");
}

void AnalyzerPanel::applyFftYAxisMode()
{
    if (!m_fftPlot) {
        return;
    }
    if (fftDensityMode()) {
        // The first completed FFT replaces this broad startup range with a
        // data-driven, whole-decade range. 0 means 1 V/√Hz and is also the
        // hard upper display limit.
        m_fftPlot->setYRange(-12.0, 0.0);
        m_fftPlot->setAxisLabels(QStringLiteral("频率 (Hz, log)"),
                                 QStringLiteral("输入噪声密度 (V/√Hz, log)"));
        m_fftPlot->setYLog10Labels(true);
    } else {
        m_fftPlot->setYRange(-170.0, 10.0);
        m_fftPlot->setAxisLabels(QStringLiteral("频率 (Hz, log)"), QStringLiteral("功率谱 (dB)"));
        m_fftPlot->setYLog10Labels(false);
    }
}

void AnalyzerPanel::syncFftUiState()
{
    const bool fftChecked = m_fftEnabledCheckbox && m_fftEnabledCheckbox->isChecked();
    const bool fftAvailable = !m_allChannelsOverviewMode;
    const bool fftVisible = fftAvailable && fftChecked;

    if (m_fftEnabledCheckbox) {
        m_fftEnabledCheckbox->setEnabled(fftAvailable);
        m_fftEnabledCheckbox->setToolTip(
            fftAvailable
                ? QStringLiteral("开启后显示频谱并计算右侧 FFT 指标")
                : QStringLiteral("256通道概览模式为节省资源锁定关闭 FFT"));
    }
    if (m_fftPointsCombo) {
        m_fftPointsCombo->setEnabled(fftVisible);
    }
    if (m_fftWindowCombo) {
        m_fftWindowCombo->setEnabled(fftVisible);
    }
    if (m_fftDcBinsSpin) {
        m_fftDcBinsSpin->setEnabled(fftVisible);
    }
    if (m_fftSigBinsSpin) {
        m_fftSigBinsSpin->setEnabled(fftVisible);
    }
    if (m_fftBandwidthSpin) {
        m_fftBandwidthSpin->setEnabled(fftVisible);
    }
    if (m_irnGainSpin) {
        m_irnGainSpin->setEnabled(fftVisible);
    }
    if (m_statsSourceCombo) {
        m_statsSourceCombo->setEnabled(fftAvailable);
    }
    if (m_fftPlot) {
        m_fftPlot->setVisible(fftVisible);
    }
    if (m_fftStatsPanel) {
        m_fftStatsPanel->setVisible(fftVisible);
    }

    updateModeStatus();
}

void AnalyzerPanel::updateModeStatus()
{
    const bool fftChecked = m_fftEnabledCheckbox && m_fftEnabledCheckbox->isChecked();
    if (m_allChannelsOverviewMode) {
        setBadgeLine(m_modeStatusLabel,
                     QStringLiteral("模式: 256CH概览 | FFT锁定关闭"),
                     "warn");
    } else {
        setBadgeLine(m_modeStatusLabel,
                     fftChecked
                         ? QStringLiteral("模式: 普通分析 | FFT ON")
                         : QStringLiteral("模式: 普通分析 | FFT OFF"),
                     fftChecked ? "ok" : "warn");
    }

    if (m_btnAllChannelsOverview) {
        m_btnAllChannelsOverview->setText(m_allChannelsOverviewMode
                                              ? QStringLiteral("退出256概览")
                                              : QStringLiteral("256通道概览"));
        m_btnAllChannelsOverview->setProperty("variant", m_allChannelsOverviewMode ? "primary" : "secondary");
        m_btnAllChannelsOverview->style()->unpolish(m_btnAllChannelsOverview);
        m_btnAllChannelsOverview->style()->polish(m_btnAllChannelsOverview);
    }
}

int AnalyzerPanel::maxSelectableChannels() const
{
    return m_allChannelsOverviewMode ? kAllChannelCount : kMaxPlottedChannels;
}

void AnalyzerPanel::setAllChannelsOverviewMode(bool enabled)
{
    if (m_btnAllChannelsOverview && m_btnAllChannelsOverview->isChecked() != enabled) {
        const QSignalBlocker blocker(m_btnAllChannelsOverview);
        m_btnAllChannelsOverview->setChecked(enabled);
    }

    if (m_allChannelsOverviewMode == enabled) {
        updateModeStatus();
        return;
    }
    const bool restartAfterChange = m_isRunning && m_hub && m_hub->isRunning();
    if (m_isRunning) {
        stopAll();
    }

    m_allChannelsOverviewMode = enabled;
    if (enabled) {
        m_overviewSavedRefreshHz = m_refreshSpin ? m_refreshSpin->value() : 10.0;
        m_overviewSavedWavePoints = m_wavePointsSpin ? m_wavePointsSpin->value() : 8000;
        m_overviewSavedFftEnabled = m_fftEnabledCheckbox ? m_fftEnabledCheckbox->isChecked() : true;
        m_overviewSavedChannels = m_channelsEdit ? m_channelsEdit->text().trimmed() : QStringLiteral("239,240");

        if (m_channelsEdit) {
            m_channelsEdit->setText(QStringLiteral("0-255"));
        }
        if (m_refreshSpin) {
            const QSignalBlocker blocker(m_refreshSpin);
            m_refreshSpin->setValue(kAllChannelRefreshHz);
            m_refreshSpin->setEnabled(false);
        }
        if (m_wavePointsSpin) {
            const QSignalBlocker blocker(m_wavePointsSpin);
            m_wavePointsSpin->setValue(kAllChannelWavePoints);
            m_wavePointsSpin->setEnabled(false);
        }
        if (m_tdmCheckbox) {
            const QSignalBlocker blocker(m_tdmCheckbox);
            m_tdmCheckbox->setChecked(false);
            m_tdmCheckbox->setEnabled(false);
        }
        if (m_tdmPhaseCombo) {
            m_tdmPhaseCombo->setEnabled(false);
        }
        if (m_fftEnabledCheckbox) {
            const QSignalBlocker blocker(m_fftEnabledCheckbox);
            m_fftEnabledCheckbox->setChecked(false);
        }
        if (m_wavePlot) {
            m_wavePlot->setMaxRenderPoints(kAllChannelRenderPoints);
            m_wavePlot->setDenseGrid(false);
        }
        syncFftUiState();
        resetFftStats();
        setStatusLine(m_statusLabel,
                      QStringLiteral("状态: 256通道概览模式 | 0.5 Hz刷新 | 1024点窗口 | FFT关闭"),
                      "warn");
        scheduleSaveAnalyzerConfig();
    } else {
        if (m_channelsEdit) {
            const QSignalBlocker blocker(m_channelsEdit);
            m_channelsEdit->setText(m_overviewSavedChannels);
        }
        if (m_refreshSpin) {
            m_refreshSpin->setEnabled(true);
            m_refreshSpin->setValue(qMax(kAllChannelRefreshHz, m_overviewSavedRefreshHz));
        }
        if (m_wavePointsSpin) {
            m_wavePointsSpin->setEnabled(true);
            m_wavePointsSpin->setValue(m_overviewSavedWavePoints);
        }
        // Restore from the global context (it may have changed from another
        // page while this page was in overview mode).
        const bool tdmEnabled =
            m_hub && m_hub->tdmContext() && m_hub->tdmContext()->enabled();
        const bool tdmEvenFirst =
            !m_hub || !m_hub->tdmContext() || m_hub->tdmContext()->pair02();
        if (m_tdmCheckbox) {
            const QSignalBlocker blocker(m_tdmCheckbox);
            m_tdmCheckbox->setEnabled(true);
            m_tdmCheckbox->setChecked(tdmEnabled);
        }
        if (m_tdmPhaseCombo) {
            const QSignalBlocker blocker(m_tdmPhaseCombo);
            m_tdmPhaseCombo->setCurrentIndex(tdmEvenFirst ? 0 : 1);
            m_tdmPhaseCombo->setEnabled(tdmEnabled);
        }
        if (m_fftEnabledCheckbox) {
            const QSignalBlocker blocker(m_fftEnabledCheckbox);
            m_fftEnabledCheckbox->setChecked(m_overviewSavedFftEnabled);
        }
        if (m_wavePlot) {
            m_wavePlot->setMaxRenderPoints(0);
            m_wavePlot->setDenseGrid(true);
        }
        syncFftUiState();
        resetFftStats();
        setStatusLine(m_statusLabel,
                      QStringLiteral("状态: 已退出256通道概览模式，普通分析最多2通道"),
                      "info");
        scheduleSaveAnalyzerConfig();
    }
    if (restartAfterChange) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_shuttingDown.load() && m_hub && m_hub->isRunning() && !m_isRunning) {
                startLive();
            }
        });
    }
}

int AnalyzerPanel::fftPointCount() const
{
    return m_fftPointsCombo
               ? qMax(256, m_fftPointsCombo->currentData().toInt())
               : qMax(256, m_cfg.fftPoints);
}

void AnalyzerPanel::restartPipelineIfRunning()
{
    if (!m_isRunning || !m_hub || !m_hub->isRunning() || m_shuttingDown.load()) {
        return;
    }
    stopAll();
    QTimer::singleShot(0, this, [this]() {
        if (!m_shuttingDown.load() && m_hub && m_hub->isRunning() && !m_isRunning) {
            startLive();
        }
    });
}

void AnalyzerPanel::updateStatsSourceChoices(const QStringList &labels)
{
    if (!m_statsSourceCombo || labels.isEmpty()) {
        return;
    }

    QString current = m_statsSourceCombo->currentText();
    if (current.isEmpty() && !m_cfg.statsSource.isEmpty()) {
        current = m_cfg.statsSource;
    }
    QStringList existing;
    for (int i = 0; i < m_statsSourceCombo->count(); ++i) {
        existing << m_statsSourceCombo->itemText(i);
    }
    if (existing == labels) {
        return;
    }

    if (!labels.contains(current)) {
        current = labels.first();
    }
    const QSignalBlocker blocker(m_statsSourceCombo);
    m_statsSourceCombo->clear();
    m_statsSourceCombo->addItems(labels);
    m_statsSourceCombo->setCurrentText(current);
    resetFftStats();
}

QString AnalyzerPanel::selectedStatsSourceLabel(const QStringList &labels) const
{
    if (labels.isEmpty()) {
        return {};
    }
    const QString selected = m_statsSourceCombo ? m_statsSourceCombo->currentText() : QString();
    return labels.contains(selected) ? selected : labels.first();
}

void AnalyzerPanel::stopAll() {
    if (m_hub) {
        m_hub->removeSubscriber(m_rawQueue);
    }
    if (m_stopFlag) {
        m_stopFlag->store(true);
    }
    if (m_rawQueue) {
        m_rawQueue->wakeAll();
    }
    if (m_timer.isActive()) {
        m_timer.stop();
    }

    m_isRunning = false;
    m_isPaused = false;
    m_firstRefreshPending = false;
    m_processEnabledAtomic.store(false);
    if (m_fsSpin) {
        m_fsSpin->setEnabled(true);
    }

    bool allStopped = true;
    if (m_sorter) {
        // The sorter's queue-pop timeout is 500ms; wait past it so a worker
        // idling in pop() gets a chance to observe the stop flag.
        if (!m_sorter->wait(600)) {
            m_rawQueue->wakeAll();
            m_sorter->wait(200);
        }
        if (!m_sorter->isRunning()) {
            m_sorter->deleteLater();
            m_sorter = nullptr;
        } else {
            allStopped = false;
        }
    }

    if (m_saveDebounceTimer.isActive()) {
        m_saveDebounceTimer.stop();
        saveAnalyzerConfigNow();
    }
    setStatusLine(m_statusLabel,
                  allStopped ? QStringLiteral("状态: 已停止")
                             : QStringLiteral("错误: 后台线程未能及时停止，请勿立即退出程序"),
                  allStopped ? "warn" : "error");
}

void AnalyzerPanel::updatePlots() {
    if (!m_isRunning) {
        return;
    }
    if (m_hub) {
        const SessionHub::Statistics stats = m_hub->statistics();
        m_rxBytes = stats.receivedBytes;
        m_droppedFrames =
            stats.ingressDroppedFrames + stats.subscriberDroppedFrames;
    }
    if (m_stopFlag && m_stopFlag->load()) {
        stopAll();
        return;
    }
    if (m_isPaused) {
        return;
    }

    if (m_channels.isEmpty()) {
        return;
    }

    const bool allOverview = m_allChannelsOverviewMode;
    const bool tdmMode = !allOverview && m_tdmCheckbox && m_tdmCheckbox->isChecked();
    const bool fftEnabled = !allOverview && (!m_fftEnabledCheckbox || m_fftEnabledCheckbox->isChecked());
    const bool evenSampleIsFirstLocalEle = !m_tdmPhaseCombo || m_tdmPhaseCombo->currentData().toInt() == 0;
    const int baseWavePoints = allOverview ? kAllChannelWavePoints : m_wavePointsSpin->value();
    const int waveN = tdmMode ? baseWavePoints * kTdmPhaseCount : baseWavePoints;
    const int fftN = tdmMode ? fftPointCount() * kTdmPhaseCount : fftPointCount();
    const double fs = m_fsSpin->value();
    const double plotFs = tdmMode ? fs / kTdmPhaseCount : fs;
    const bool doFft = fftEnabled && !m_fftBusy.load() && !m_shuttingDown.load();
    m_wavePlot->setXRange(0.0, static_cast<double>(qMax(1, waveN - 1)) / qMax(1.0, fs));
    if (fftEnabled) {
        // fftN counts raw samples before the four-phase TDM split; each ELE's FFT runs
        // over fftPointCount() samples at plotFs, so that is the bin spacing.
        const double firstPositiveBinHz =
            plotFs / static_cast<double>(qMax(1, fftPointCount()));
        m_fftPlot->setXRange(firstPositiveBinHz, plotFs / 2.0);
    }

    QVector<WaveformWidget::PlotSeries> waveSeries;
    int maxBuffered = 0;

    // Collect FFT input data (to be processed off-thread)
    QVector<FftInput> fftInputs;

    int colorIdx = 0;
    for (int gIdx = 0; gIdx < m_groups.size(); ++gIdx) {
        const QVector<int> &grp = m_groups[gIdx];
        for (int ch : grp) {
            const std::shared_ptr<RingBuffer> buf = m_dataStore.sharedBuffer(ch);
            if (!buf) {
                continue;
            }

            auto wavePair = buf->getLatest(waveN);
            const QVector<qint64> &waveIndices = wavePair.first;
            const QVector<qint32> &waveRaw = wavePair.second;
            maxBuffered = qMax(maxBuffered, waveRaw.size());
            if (!waveRaw.isEmpty()) {
                if (tdmMode) {
                    const QVector<TdmSeries> split = splitTdmSeries(ch, waveIndices, waveRaw, evenSampleIsFirstLocalEle);
                    for (int part = 0; part < split.size(); ++part) {
                        if (split[part].data.isEmpty()) {
                            continue;
                        }
                        WaveformWidget::PlotSeries s;
                        s.data = split[part].data;
                        for (qint64 index : split[part].indices)
                            s.xData.push_back((index - waveIndices.first()) / fs);
                        s.color = QColor::fromHsv(((colorIdx + part) * 47) % 360, 220, 255);
                        s.label = split[part].label;
                        waveSeries.push_back(s);
                    }
                } else {
                    WaveformWidget::PlotSeries s;
                    s.data = convertAdcValues(waveRaw);
                    for (qint64 index : waveIndices)
                        s.xData.push_back((index - waveIndices.first()) / fs);
                    s.color = QColor::fromHsv((colorIdx * 47) % 360, 220, 255);
                    s.label = QStringLiteral("CH%1").arg(ch);
                    waveSeries.push_back(s);
                }
            }

            const int channelColorBase = colorIdx;
            colorIdx += tdmMode ? 2 : 1;

            if (!doFft || m_fftBusy.load()) {
                continue;
            }

            auto fftPair = buf->getLatestExact(fftN);
            const QVector<qint64> &fftIndices = fftPair.first;
            const QVector<qint32> &fftRaw = fftPair.second;
            if (fftRaw.isEmpty()) {
                continue;
            }

            if (tdmMode) {
                const QVector<TdmSeries> split = splitTdmSeries(ch, fftIndices, fftRaw, evenSampleIsFirstLocalEle);
                for (int part = 0; part < split.size(); ++part) {
                    if (split[part].data.isEmpty()) {
                        continue;
                    }
                    fftInputs.append({ch,
                                      split[part].data,
                                      QColor::fromHsv(((channelColorBase + part) * 47) % 360, 220, 255),
                                      split[part].label,
                                      false});
                }
            } else {
                fftInputs.append({ch,
                                  convertAdcValues(fftRaw),
                                  QColor::fromHsv((channelColorBase * 47) % 360, 220, 255),
                                  QStringLiteral("CH%1").arg(ch),
                                  false});
            }
        }
    }

    QStringList outputLabels;
    outputLabels.reserve(waveSeries.size());
    for (const auto &series : waveSeries) {
        outputLabels << series.label;
    }
    if (!allOverview) {
        updateStatsSourceChoices(outputLabels);
    } else if (m_statsSourceCombo
               && (m_statsSourceCombo->count() != 1
                   || m_statsSourceCombo->itemText(0) != QStringLiteral("256CH概览"))) {
        const QSignalBlocker blocker(m_statsSourceCombo);
        m_statsSourceCombo->clear();
        m_statsSourceCombo->addItem(QStringLiteral("256CH概览"));
    }
    const QString statsSourceLabel = allOverview ? QStringLiteral("256CH概览") : selectedStatsSourceLabel(outputLabels);
    bool hasSelectedFftInput = false;
    for (FftInput &input : fftInputs) {
        input.statsSource = input.label == statsSourceLabel;
        hasSelectedFftInput = hasSelectedFftInput || input.statsSource;
    }
    if (!hasSelectedFftInput && !fftInputs.isEmpty()) {
        fftInputs[0].statsSource = true;
    }
    if (m_fftStats) {
        m_fftStats->setSourceLabel(statsSourceLabel);
    }

    if (!waveSeries.isEmpty()) {
        m_wavePlot->setSeries(waveSeries);
        if (m_timeStats) {
            m_timeStats->updateStats(summarizeMultiChannelStats(waveSeries));
        }
    } else if (m_timeStats) {
        m_timeStats->reset();
    }
    m_lastBufferSamples = maxBuffered;
    setStatusLine(m_statusLabel,
                  QStringLiteral("状态: RX %1 B | frames %2 | dropped %3 | leftover %4 B | buffered %5%6")
                      .arg(m_rxBytes)
                      .arg(m_parsedFrames)
                      .arg(m_droppedFrames)
                      .arg(m_leftoverBytes)
                      .arg(m_lastBufferSamples)
                      .arg(allOverview ? QStringLiteral(" | 256CH概览 0.5Hz | FFT关闭")
                                       : (fftEnabled ? QString() : QStringLiteral(" | FFT关闭"))),
                  (m_droppedFrames > 0 || !fftEnabled) ? "warn" : "info");

    // Launch async FFT for all channels (off UI thread)
    if (doFft && !m_fftBusy.load() && !fftInputs.isEmpty()) {
        m_fftBusy.store(true);
        const double capturedFs = plotFs;
        FftAnalysisConfig fftCfg;
        fftCfg.window = m_fftWindowCombo ? m_fftWindowCombo->currentData().toString() : QStringLiteral("hann");
        fftCfg.dcBins = m_fftDcBinsSpin ? m_fftDcBinsSpin->value() : 5;
        fftCfg.signalBins = m_fftSigBinsSpin ? m_fftSigBinsSpin->value() : 5;
        const double requestedBandwidth = m_fftBandwidthSpin ? m_fftBandwidthSpin->value() : (plotFs / 2.0);
        const double minBandwidth = plotFs / static_cast<double>(qMax(1, fftN));
        const double capturedBandwidth = qBound(minBandwidth, requestedBandwidth, plotFs / 2.0);
        const double capturedIrnGain = qMax(0.001, m_irnGainSpin ? m_irnGainSpin->value() : m_cfg.irnGain);
        const bool capturedDensityMode = fftDensityMode();
        const quint64 capturedEpoch = m_timelineEpoch.load();
        const quint64 capturedGeneration = m_analysisGeneration.load();

        QPointer<AnalyzerPanel> self(this);
        m_fftFuture = QtConcurrent::run([self, fftInputs, capturedFs, capturedBandwidth, capturedIrnGain, capturedDensityMode, statsSourceLabel, fftCfg, capturedEpoch, capturedGeneration]() {
            QVector<WaveformWidget::PlotSeries> fftResults;
            bool hasStats = false;
            QString statsError;
            FftStats firstStats;
            double densityLogMin = std::numeric_limits<double>::infinity();
            double densityLogMax = -std::numeric_limits<double>::infinity();

            for (const auto &input : fftInputs) {
                if (!self || self->m_shuttingDown.load()) {
                    return;
                }
                const SndrResult r = calSndr(input.data, capturedFs, capturedBandwidth, fftCfg);
                if (!r.ok && input.statsSource) statsError = r.error;
                if (!r.fftData.isEmpty()) {
                    QVector<double> fftDb;
                    fftDb.reserve(r.fftData.size());
                    if (capturedDensityMode) {
                        // fftData is PSD in V²/Hz (one-sided pairs not yet
                        // doubled): input-referred amplitude noise density
                        // = sqrt(2·PSD)/gain, plotted as log10(V/√Hz).
                        for (int bin = 0; bin < r.fftData.size(); ++bin) {
                            const double p = r.fftData[bin];
                            const double density =
                                std::sqrt(std::max(2.0 * p, 0.0)) / capturedIrnGain;
                            const double logDensity = std::log10(std::max(density, 1e-12));
                            fftDb.push_back(logDensity);
                            // DC is not rendered on the logarithmic X axis and
                            // must not stretch the visible noise-floor range.
                            if (bin > 0 && std::isfinite(logDensity)) {
                                densityLogMin = std::min(densityLogMin, logDensity);
                                densityLogMax = std::max(densityLogMax, logDensity);
                            }
                        }
                    } else {
                        for (double p : r.fftData) {
                            fftDb.push_back(10.0 * std::log10(std::max(p, 1e-18)));
                        }
                    }
                    WaveformWidget::PlotSeries s;
                    s.data = fftDb;
                    s.xData = r.fftFreq;
                    s.color = input.color;
                    s.label = input.label;
                    fftResults.push_back(s);

                    if (input.statsSource && r.ok) {
                        firstStats.fin = r.fin;
                        firstStats.sndr = r.sndrDb;
                        firstStats.enob = r.enob;
                        firstStats.snr = r.snrDb;
                        firstStats.sfdr = r.sfdrDb;
                        firstStats.thd = r.thdDb;
                        firstStats.thdOdd = r.thdOddDb;
                        firstStats.thdEven = r.thdEvenDb;
                        firstStats.irnPower = r.irnPowerDb;
                        firstStats.irn = r.irn / capturedIrnGain * 1e6;
                        firstStats.fom = r.fomDb;
                        hasStats = true;
                    }
                }
            }

            if (!self) {
                return;
            }
            double densityYMin = -12.0;
            double densityYMax = 0.0;
            if (capturedDensityMode
                && std::isfinite(densityLogMin)
                && std::isfinite(densityLogMax)) {
                densityYMax = std::min(0.0, std::ceil(densityLogMax));
                densityYMin = std::floor(densityLogMin);
                // Keep at least four decades visible and avoid sub-pV display
                // ranges generated by numerical round-off in empty bins.
                densityYMin = std::max(-12.0, std::min(densityYMin, densityYMax - 4.0));
                if (densityYMax - densityYMin < 4.0) {
                    densityYMax = std::min(0.0, densityYMin + 4.0);
                }
            }

            QMetaObject::invokeMethod(self, [self, fftResults, hasStats, firstStats, statsError, statsSourceLabel, capturedEpoch, capturedGeneration, capturedDensityMode, densityYMin, densityYMax]() {
                if (!self) {
                    return;
                }
                if (self->m_shuttingDown.load()) {
                    self->m_fftBusy.store(false);
                    return;
                }
                if (self->m_timelineEpoch.load() != capturedEpoch) {
                    self->m_fftBusy.store(false);
                    return;
                }
                // One generation counter covers every semantic change (TDM
                // mode/phase, stats source, fs, FFT settings, display unit):
                // results computed under old settings are discarded whole.
                if (self->m_analysisGeneration.load() != capturedGeneration) {
                    self->m_fftBusy.store(false);
                    return;
                }
                const bool stillEnabled = !self->m_fftEnabledCheckbox || self->m_fftEnabledCheckbox->isChecked();
                if (!stillEnabled) {
                    self->m_fftBusy.store(false);
                    return;
                }
                if (capturedDensityMode && !fftResults.isEmpty()) {
                    self->m_fftPlot->setYRange(densityYMin, densityYMax);
                }
                self->m_fftPlot->setSeries(fftResults);
                if (hasStats && self->m_fftStats) {
                    self->m_fftStats->setSourceLabel(statsSourceLabel);
                    self->m_fftStats->updateStats(firstStats);
                } else if (self->m_fftStats) {
                    self->m_fftStats->invalidateCurrent(statsError.isEmpty()
                        ? QStringLiteral("当前信号无法计算 FFT 指标") : statsError);
                }
                self->m_fftBusy.store(false);
            }, Qt::QueuedConnection);
        });
    }
}

void AnalyzerPanel::stopAcquisition() {
    stopAll();
}

void AnalyzerPanel::shutdown() {
    m_shuttingDown.store(true);
    if (m_saveDebounceTimer.isActive()) {
        m_saveDebounceTimer.stop();
        saveAnalyzerConfigNow();
    }
    stopAll();
    for (QThread *thread : {static_cast<QThread *>(m_sorter)}) {
        if (thread && thread->isRunning()) {
            thread->wait();
        }
    }
    if (m_fftFuture.isRunning()) {
        m_fftFuture.waitForFinished();
    }
    m_fftBusy.store(false);
}

void AnalyzerPanel::setSessionHub(SessionHub *hub) {
    m_hub = hub;
    if (!m_hub) return;
    connect(m_hub, &SessionHub::stateChanged, this, [this](SessionHub::State state) {
        if (!m_isRunning && (state == SessionHub::State::Idle || state == SessionHub::State::Error)) {
            const QSignalBlocker blocker(m_fsSpin);
            m_fsSpin->setValue(m_cfg.samplingRate);
            m_fsSpin->setEnabled(true);
        }
    });
    if (TdmContext *tdm = m_hub->tdmContext()) {
        applyGlobalTdmState(tdm->enabled(), tdm->pair02());
        connect(tdm, &TdmContext::changed, this, &AnalyzerPanel::applyGlobalTdmState);
    }
    connect(m_hub, &SessionHub::connectionStateChanged, this, [this](bool connected) {
        if (!connected && m_isRunning) {
            setStatusLine(m_statusLabel, QStringLiteral("状态: 采集连接已断开"), "warn");
        }
    });
    connect(m_hub,
            &SessionHub::timelineReset,
            this,
            [this](quint64 epoch, qint64 targetFrame) {
        resetTimelineState(epoch, targetFrame);
    });
}

void AnalyzerPanel::applyGlobalTdmState(bool enabled, bool evenFirst) {
    // Overview mode forces the TDM display off locally; the global state is
    // re-applied when the overview is exited.
    if (m_allChannelsOverviewMode) {
        return;
    }
    if (m_tdmCheckbox) {
        const QSignalBlocker blocker(m_tdmCheckbox);
        m_tdmCheckbox->setChecked(enabled);
    }
    if (m_tdmPhaseCombo) {
        const QSignalBlocker blocker(m_tdmPhaseCombo);
        m_tdmPhaseCombo->setCurrentIndex(evenFirst ? 0 : 1);
        m_tdmPhaseCombo->setEnabled(enabled);
    }
    if (m_fftBandwidthSpin && m_fsSpin) {
        const double nyquist = m_fsSpin->value() /
                               (enabled ? (2.0 * kTdmPhaseCount) : 2.0);
        const QSignalBlocker blocker(m_fftBandwidthSpin);
        m_fftBandwidthSpin->setMaximum(nyquist);
        if (m_fftBandwidthSpin->value() > nyquist) {
            m_fftBandwidthSpin->setValue(nyquist);
        }
    }
    resetFftStats();
}

void AnalyzerPanel::resetTimelineState(quint64 epoch, qint64 targetFrame)
{
    m_timelineEpoch.store(epoch);
    // Seek barrier: SorterWorker keeps running and drops stale chunks itself
    // (epoch check); pending FFT results are discarded by the same epoch on
    // arrival. Only the store and plots need clearing here.
    if (m_rawQueue) m_rawQueue->clear();
    if (!m_channels.isEmpty()) {
        const int maxPoints = m_allChannelsOverviewMode
                                  ? kAllChannelWavePoints
                                  : qMax(m_wavePointsSpin->value(), fftPointCount());
        const int capacity = m_allChannelsOverviewMode
                                 ? qMax(maxPoints * 4, 4096)
                                 : qMax(maxPoints * 8, 65536);
        m_dataStore.reset(m_channels, capacity, targetFrame, epoch);
    }
    resetFftStats();
    if (m_wavePlot) m_wavePlot->setSeries({});
    if (m_fftPlot) m_fftPlot->setSeries({});
    if (m_timeStats) m_timeStats->reset();
    m_parsedFrames = 0;
    m_leftoverBytes = 0;
    m_lastBufferSamples = 0;
    m_firstRefreshPending = m_isRunning;
}

void AnalyzerPanel::setPlotTheme(const QMap<QString, QString> &palette) {
    m_wavePlot->setPaletteColors(palette);
    m_fftPlot->setPaletteColors(palette);
}

void AnalyzerPanel::onNetworkChanged(const QString &host, int port) {
    m_netLabel->setText(QStringLiteral("%1 控制:%2 数据:%3")
                            .arg(host)
                            .arg(port)
                            .arg(m_networkState->dataPort()));
    m_netStateLabel->setText(QStringLiteral("已同步"));
}

void AnalyzerPanel::onGetChannelAddress() {
    QVector<int> channels = m_channelState->selectedGlobalChannels();
    if (channels.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("读取通道地址"), QStringLiteral("请先在1024通道分布页面选择通道"));
        return;
    }
    if (m_allChannelsOverviewMode && channels.size() <= kMaxPlottedChannels) {
        setAllChannelsOverviewMode(false);
    }
    QStringList text;
    for (int ch : channels) {
        text << QString::number(ch);
    }
    m_channelsEdit->setText(text.join(','));
    scheduleSaveAnalyzerConfig();
    restartPipelineIfRunning();
    setStatusLine(m_statusLabel, QStringLiteral("状态: 已从通道分布载入通道"), "ok");
}

void AnalyzerPanel::checkNetworkConnectivity() {
    QTcpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(m_networkState->host(), m_networkState->port());
    if (socket.waitForConnected(800)) {
        m_netStateLabel->setText(QStringLiteral("可连接"));
        setStatusLine(m_statusLabel,
                      QStringLiteral("状态: 控制口可连接 %1:%2，数据口启动实时采集时使用 %3")
                          .arg(m_networkState->host())
                          .arg(m_networkState->port())
                          .arg(m_networkState->dataPort()),
                      "ok");
    } else {
        m_netStateLabel->setText(QStringLiteral("不可连接"));
        setStatusLine(m_statusLabel,
                      QStringLiteral("状态: 网络检测失败 %1:%2 (%3)")
                          .arg(m_networkState->host())
                          .arg(m_networkState->port())
                          .arg(socket.errorString()),
                      "error");
    }
}

}  // namespace ccv2
