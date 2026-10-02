#include "ui/realtime_plot_panel.h"

#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSet>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>
#include <limits>
#include <stdexcept>

#include "core/constants.h"
#include "core/channel_routing.h"
#include "core/tdm_context.h"
#include "network/socket_receiver2.h"
#include "service/session_hub.h"
#include "ui/widgets/activity_map_view.h"
#include "ui/widgets/stack_waveform_view.h"
#include "ui/waveform_widget.h"

namespace ccv2 {

namespace {

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

struct TdmSeries {
    QVector<double> data;
    int localEle{-1};
    int globalEle{-1};
    QString label;
};

QVector<TdmSeries> splitTdmSeries(int globalChannel,
                                  qint64 firstSampleIndex,
                                  const QVector<double> &samples,
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

    first.data.reserve((samples.size() + kTdmPhaseCount - 1) / kTdmPhaseCount);
    second.data.reserve((samples.size() + kTdmPhaseCount - 1) / kTdmPhaseCount);
    for (int i = 0; i < samples.size(); ++i) {
        const int phase = tdmPhaseForFrame(firstSampleIndex + i);
        if (phase == pair.first) {
            first.data.push_back(samples[i]);
        } else if (phase == pair.second) {
            second.data.push_back(samples[i]);
        }
    }
    return {first, second};
}

QColor colorForTdmLocalEle(int localEle)
{
    switch (localEle) {
    case 0:
        return QColor(QStringLiteral("#ff5a45"));
    case 1:
        return QColor(QStringLiteral("#39c5ff"));
    case 2:
        return QColor(QStringLiteral("#ffd33d"));
    case 3:
        return QColor(QStringLiteral("#d76bff"));
    default:
        return QColor(QStringLiteral("#63e6be"));
    }
}

QColor colorForStackIndex(int index, int count)
{
    const qreal h = std::fmod(198.0 + index * 360.0 / qMax(1, count) * 0.62, 360.0);
    return QColor::fromHslF(h / 360.0, 0.62, 0.58);
}

QString metricVoltageText(double volts)
{
    if (volts >= 1.0) {
        return QStringLiteral("%1 V").arg(volts, 0, 'f', 3);
    }
    return QStringLiteral("%1 mV").arg(volts * 1000.0, 0, 'f', 2);
}

enum DisplaySignalMode {
    DisplayDc = 0,
    DisplayAc = 1,
    DisplayHighpass = 2,
};

enum StackYScaleMode {
    StackFixedY = 0,
    StackAdaptiveY = 1,
};

QString defaultRealtimeChannelsText()
{
    QStringList channels;
    channels.reserve(32);
    for (int i = 0; i < 32; ++i) {
        channels << QString::number(i * 8);
    }
    return channels.join(QLatin1Char(','));
}

QString channelsToText(const QVector<int> &channels)
{
    QStringList parts;
    parts.reserve(channels.size());
    for (int ch : channels) {
        parts << QString::number(ch);
    }
    return parts.join(QLatin1Char(','));
}

QVector<int> channelsFromText(const QString &text, int maximumCount)
{
    QVector<int> channels;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QSet<int> unique;
    for (const QString &part : parts) {
        bool ok = false;
        const int ch = part.trimmed().toInt(&ok);
        if (!ok || ch < 0 || ch >= kChannelsTotal || unique.contains(ch)) {
            return {};
        }
        unique.insert(ch);
        channels.push_back(ch);
        if (channels.size() > maximumCount) {
            return {};
        }
    }
    return channels;
}

void setComboCurrentData(QComboBox *combo, int value)
{
    if (!combo) {
        return;
    }
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i).toInt() == value) {
            combo->setCurrentIndex(i);
            return;
        }
    }
}

int viewModeFromConfig(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("origin") || t == QStringLiteral("0")) {
        return 0;
    }
    return 1;
}

QString viewModeToConfig(int mode)
{
    return mode == 0 ? QStringLiteral("origin") : QStringLiteral("stack");
}

int signalModeFromConfig(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("ac") || t == QStringLiteral("1")) {
        return DisplayAc;
    }
    if (t == QStringLiteral("highpass") || t == QStringLiteral("hp") || t == QStringLiteral("2")) {
        return DisplayHighpass;
    }
    return DisplayDc;
}

QString signalModeToConfig(int mode)
{
    switch (mode) {
    case DisplayAc:
        return QStringLiteral("ac");
    case DisplayHighpass:
        return QStringLiteral("highpass");
    default:
        return QStringLiteral("dc");
    }
}

int stackYScaleModeFromConfig(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("adaptive") || t == QStringLiteral("auto") || t == QStringLiteral("1")) {
        return StackAdaptiveY;
    }
    return StackFixedY;
}

QString stackYScaleModeToConfig(int mode)
{
    return mode == StackAdaptiveY ? QStringLiteral("adaptive") : QStringLiteral("fixed");
}

int heatmapMetricFromConfig(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("p2p") || t == QStringLiteral("1")) {
        return ActivityMapView::MetricP2p;
    }
    if (t == QStringLiteral("mean") || t == QStringLiteral("2")) {
        return ActivityMapView::MetricMean;
    }
    if (t == QStringLiteral("saturation") || t == QStringLiteral("sat") || t == QStringLiteral("3")) {
        return ActivityMapView::MetricSaturation;
    }
    return ActivityMapView::MetricRms;
}

QString heatmapMetricToConfig(int metric)
{
    switch (metric) {
    case ActivityMapView::MetricP2p:
        return QStringLiteral("p2p");
    case ActivityMapView::MetricMean:
        return QStringLiteral("mean");
    case ActivityMapView::MetricSaturation:
        return QStringLiteral("saturation");
    default:
        return QStringLiteral("rms");
    }
}

QVector<double> transformDisplayData(const QVector<double> &data, int mode, double samplingRate)
{
    if (mode == DisplayDc || data.isEmpty()) {
        return data;
    }

    QVector<double> out;
    out.reserve(data.size());

    if (mode == DisplayAc) {
        double sum = 0.0;
        int count = 0;
        for (double v : data) {
            if (!std::isfinite(v)) {
                continue;
            }
            sum += v;
            ++count;
        }
        const double mean = count > 0 ? sum / static_cast<double>(count) : 0.0;
        for (double v : data) {
            out.push_back(std::isfinite(v) ? v - mean : std::numeric_limits<double>::quiet_NaN());
        }
        return out;
    }

    constexpr double cutoffHz = 300.0;
    const double fs = qMax(1.0, samplingRate);
    const double dt = 1.0 / fs;
    constexpr double pi = 3.14159265358979323846;
    const double rc = 1.0 / (2.0 * pi * cutoffHz);
    const double alpha = rc / (rc + dt);
    double yPrev = 0.0;
    double xPrev = 0.0;
    bool initialized = false;
    for (int i = 0; i < data.size(); ++i) {
        const double x = data[i];
        if (!std::isfinite(x)) {
            out.push_back(std::numeric_limits<double>::quiet_NaN());
            initialized = false;
            continue;
        }
        if (!initialized) {
            xPrev = x;
            yPrev = 0.0;
            out.push_back(0.0);
            initialized = true;
            continue;
        }
        const double y = alpha * (yPrev + x - xPrev);
        out.push_back(y);
        yPrev = y;
        xPrev = x;
    }
    return out;
}

QString displayModeSuffix(int mode)
{
    switch (mode) {
    case DisplayAc:
        return QStringLiteral("AC");
    case DisplayHighpass:
        return QStringLiteral("HP");
    default:
        return QStringLiteral("DC");
    }
}

}  // namespace

RealtimePlotPanel::RealtimePlotPanel(ConfigManager *cfgMgr,
                                                                         NetworkState *networkState,
                                                                         ChannelAddressState *channelState,
                                                                         QWidget *parent)
    : QWidget(parent),
      m_cfgMgr(cfgMgr),
            m_networkState(networkState),
            m_channelState(channelState),
      m_rawQueue(std::make_shared<ThreadSafeQueue<QByteArray>>(128)),
      m_streamState(std::make_shared<RealtimeStreamState>()),
      m_stopFlag(std::make_unique<std::atomic_bool>(false)) {
    const ConfigMap appCfg = cfgMgr->load();
    const ConfigSection net = appCfg.value(QStringLiteral("Network"));
    const ConfigSection signal = appCfg.value(QStringLiteral("Signal"));
    const ConfigSection realtime = appCfg.value(QStringLiteral("Realtime"));
    m_defaultHost = net.value(QStringLiteral("host"), QStringLiteral("localhost"));
    m_samplingRate = signal.value(QStringLiteral("sampling_rate"), QStringLiteral("20000")).toDouble();
    const int fallbackPoints = qMax(128, static_cast<int>(0.06 * qMax(1.0, m_samplingRate)));
    m_samplesPerView = qMax(128,
                            realtime.value(QStringLiteral("wave_points"),
                                           signal.value(QStringLiteral("wave_points"), QString::number(fallbackPoints)))
                                .toInt());
    m_defaultCommand = realtime.value(QStringLiteral("command"), QStringLiteral("ctre")).trimmed();
    if (m_defaultCommand.isEmpty()) {
        m_defaultCommand = QStringLiteral("ctre");
    }
    m_defaultChannelsText = realtime.value(QStringLiteral("channels"), defaultRealtimeChannelsText()).trimmed();
    m_defaultRefreshHz = qBound(1,
                                realtime.value(QStringLiteral("refresh_hz"),
                                               signal.value(QStringLiteral("refresh_hz"), QStringLiteral("10")))
                                    .toInt(),
                                60);
    m_defaultViewMode = viewModeFromConfig(realtime.value(QStringLiteral("view_mode"), QStringLiteral("stack")));
    m_defaultSignalMode = signalModeFromConfig(realtime.value(QStringLiteral("signal_mode"), QStringLiteral("dc")));
    m_defaultRefMode = realtime.value(QStringLiteral("reference_mode"), QStringLiteral("0")).toInt();
    m_defaultStackYScaleMode =
        stackYScaleModeFromConfig(realtime.value(QStringLiteral("stack_y_mode"), QStringLiteral("fixed")));
    // TDM state is global ([Session] via SessionHub's TdmContext); the
    // controls are synced from it in setSessionHub.
    m_defaultHeatmapMetric = heatmapMetricFromConfig(realtime.value(QStringLiteral("heatmap_metric"), QStringLiteral("rms")));

    QVector<int> defaultChannels = channelsFromText(m_defaultChannelsText, 32);
    if (defaultChannels.isEmpty()) {
        defaultChannels = channelsFromText(defaultRealtimeChannelsText(), 32);
        m_defaultChannelsText = channelsToText(defaultChannels);
    } else {
        m_defaultChannelsText = channelsToText(defaultChannels);
    }

    m_streamState->channels = defaultChannels;
    for (int ch : defaultChannels) {
        m_streamState->buffers[ch] = QQueue<double>();
        m_streamState->totalSamples[ch] = 0;
    }

    buildUi(defaultChannels);
    applyViewLengthInternal(false);

    connect(m_networkState, &NetworkState::endpointChanged, this, &RealtimePlotPanel::onNetworkChanged);
    connect(m_networkState, &NetworkState::dataPortChanged, this, [this](int) {
        onNetworkChanged(m_networkState->host(), m_networkState->port());
    });
    onNetworkChanged(m_networkState->host(), m_networkState->port());

    connect(&m_refreshTimer, &QTimer::timeout, this, &RealtimePlotPanel::refresh);

    m_saveDebounceTimer.setSingleShot(true);
    m_saveDebounceTimer.setInterval(500);
    connect(&m_saveDebounceTimer, &QTimer::timeout, this, [this]() { saveRealtimeConfig(); });
}

void RealtimePlotPanel::scheduleSaveRealtimeConfig() {
    m_saveDebounceTimer.start();
}

void RealtimePlotPanel::buildUi(const QVector<int> &defaultChannels) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto fieldLabel = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setProperty("role", "field-label");
        return label;
    };

    // ---- Control widgets (creation) ----
    m_netLabel = new QLabel;
    m_netLabel->setProperty("role", "data-value");
    m_rtCmd = new QLineEdit(m_defaultCommand);
    m_rtChEdit = new QLineEdit;
    m_viewModeCombo = new QComboBox;
    m_viewModeCombo->addItem(QStringLiteral("Origin"), 0);
    m_viewModeCombo->addItem(QStringLiteral("Stack"), 1);
    setComboCurrentData(m_viewModeCombo, m_defaultViewMode);
    m_signalModeCombo = new QComboBox;
    m_signalModeCombo->addItem(QStringLiteral("DC"), DisplayDc);
    m_signalModeCombo->addItem(QStringLiteral("AC"), DisplayAc);
    m_signalModeCombo->addItem(QStringLiteral("Highpass"), DisplayHighpass);
    setComboCurrentData(m_signalModeCombo, m_defaultSignalMode);
    m_refModeCombo = new QComboBox;
    m_refModeCombo->addItem(QStringLiteral("无"), 0);
    m_refModeCombo->addItem(QStringLiteral("共平均 (CAR)"), 1);
    m_refModeCombo->addItem(QStringLiteral("中值"), 2);
    m_refModeCombo->setToolTip(QStringLiteral(
        "软件参考（全局）：逐帧对所有通道减去共模，\n"
        "抑制阵列共模噪声。对实时波形与实时spike页生效。"));
    setComboCurrentData(m_refModeCombo, m_defaultRefMode);
    m_stackYScaleCombo = new QComboBox;
    m_stackYScaleCombo->addItem(QStringLiteral("固定Y"), StackFixedY);
    m_stackYScaleCombo->addItem(QStringLiteral("自适应Y"), StackAdaptiveY);
    setComboCurrentData(m_stackYScaleCombo, m_defaultStackYScaleMode);

    QStringList chs;
    for (int ch : defaultChannels) {
        chs << QString::number(ch);
    }
    m_rtChEdit->setText(m_defaultChannelsText.isEmpty() ? chs.join(',') : m_defaultChannelsText);

    m_btnApply = new QPushButton(QStringLiteral("应用通道"));
    m_btnGetAddrRt = new QPushButton(QStringLiteral("从通道图载入"));
    m_btnApply->setProperty("variant", "primary");
    m_btnGetAddrRt->setProperty("variant", "secondary");

    m_tdmCheckbox = new QCheckBox(QStringLiteral("TDM分组显示"));
    m_tdmCheckbox->setToolTip(QStringLiteral(
        "全局 TDM 状态：与分析器/实时Spike页联动，\n随录制写入 session.json，回放时自动恢复。"));
    m_tdmPhaseCombo = new QComboBox;
    m_tdmPhaseCombo->addItem(QStringLiteral("显示相位 0 / 2"), 0);
    m_tdmPhaseCombo->addItem(QStringLiteral("显示相位 1 / 3"), 1);
    m_tdmCheckbox->setChecked(false);
    m_tdmPhaseCombo->setEnabled(false);

    m_rtPointsSpin = new QSpinBox;
    m_rtPointsSpin->setRange(128, 200000);
    m_rtPointsSpin->setSingleStep(128);
    m_rtPointsSpin->setValue(m_samplesPerView);
    m_refreshHzSpin = new QSpinBox;
    m_refreshHzSpin->setRange(1, 60);
    m_refreshHzSpin->setSuffix(QStringLiteral(" Hz"));
    m_refreshHzSpin->setValue(m_defaultRefreshHz);
    m_rtTimeHint = new QLabel;
    m_rtTimeHint->setProperty("role", "caption");
    m_rtTimeHint->setWordWrap(true);
    m_rtTimeHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_btnApplyAxis = new QPushButton(QStringLiteral("应用X轴"));
    m_btnApplyAxis->setProperty("variant", "secondary");

    m_rtStatus = new QLabel(QStringLiteral("状态: 空闲"));
    m_rtStatus->setProperty("role", "status-line");
    m_rtStatus->setProperty("state", "info");
    m_rtStatus->setWordWrap(true);
    m_rtStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // ---- Left side panel: vertical controls (top) + heatmap (bottom) ----
    auto *side = new QFrame;
    side->setObjectName(QStringLiteral("realtimeSidePanel"));
    side->setProperty("role", "page-control-panel");
    side->setFixedWidth(320);
    side->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto *sideCol = new QVBoxLayout(side);
    sideCol->setContentsMargins(0, 0, 0, 0);
    sideCol->setSpacing(6);

    auto *form = new QGridLayout;
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(5);
    form->setColumnStretch(1, 1);
    int fr = 0;
    auto addField = [&](const QString &label, QWidget *w) {
        form->addWidget(fieldLabel(label), fr, 0);
        form->addWidget(w, fr, 1);
        ++fr;
    };
    addField(QStringLiteral("网络"), m_netLabel);
    addField(QStringLiteral("命令"), m_rtCmd);
    addField(QStringLiteral("通道"), m_rtChEdit);
    addField(QStringLiteral("显示"), m_viewModeCombo);
    addField(QStringLiteral("信号"), m_signalModeCombo);
    addField(QStringLiteral("参考"), m_refModeCombo);
    addField(QStringLiteral("波形点数"), m_rtPointsSpin);
    addField(QStringLiteral("刷新"), m_refreshHzSpin);
    addField(QStringLiteral("Stack Y轴"), m_stackYScaleCombo);
    addField(QStringLiteral("TDM相位"), m_tdmPhaseCombo);
    sideCol->addLayout(form);
    sideCol->addWidget(m_tdmCheckbox);

    auto *chBtnRow = new QHBoxLayout;
    chBtnRow->setSpacing(6);
    chBtnRow->addWidget(m_btnApply);
    chBtnRow->addWidget(m_btnGetAddrRt);
    chBtnRow->addWidget(m_btnApplyAxis);
    sideCol->addLayout(chBtnRow);

    // V6: session run/stop lives on the global toolbar; this page is a pure
    // view driven by SessionHub state (startStream/stopStream slots).
    sideCol->addWidget(m_rtTimeHint);
    sideCol->addWidget(m_rtStatus);

    connect(m_btnApply, &QPushButton::clicked, this, &RealtimePlotPanel::applyChannels);
    connect(m_btnApplyAxis, &QPushButton::clicked, this, &RealtimePlotPanel::applyViewLength);
    connect(m_btnGetAddrRt, &QPushButton::clicked, this, &RealtimePlotPanel::onGetChannelAddress);
    connect(m_rtCmd, &QLineEdit::editingFinished, this, &RealtimePlotPanel::scheduleSaveRealtimeConfig);
    connect(m_viewModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_plotModeStack) {
            m_plotModeStack->setCurrentIndex(index == 1 ? 1 : 0);
        }
        if (applyChannelsInternal()) {
            scheduleSaveRealtimeConfig();
        }
        refresh();
    });
    connect(m_signalModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        updateStackYScale();
        updateTimeHint();
        scheduleSaveRealtimeConfig();
        refresh();
    });
    connect(m_refModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        // Global software reference — takes effect immediately for all sorters.
        if (m_hub) m_hub->setReferenceMode(m_refModeCombo->currentData().toInt());
        scheduleSaveRealtimeConfig();
    });
    connect(m_stackYScaleCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        updateStackYScale();
        updateTimeHint();
        scheduleSaveRealtimeConfig();
        refresh();
    });
    connect(m_refreshHzSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        if (m_refreshTimer.isActive()) {
            m_refreshTimer.start(refreshIntervalMs());
        }
        updateTimeHint();
        scheduleSaveRealtimeConfig();
    });
    // The widgets are mirrors of the global TdmContext: user edits go to the
    // context, and applyTdmState (driven by the context's changed signal)
    // performs the actual page update — also when the change came from
    // another page or a replay restore.
    connect(m_tdmCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_hub && m_hub->tdmContext()) {
            m_hub->tdmContext()->setEnabled(checked);
            return;
        }
        applyTdmState(checked,
                      !m_tdmPhaseCombo || m_tdmPhaseCombo->currentData().toInt() == 0);
    });
    connect(m_tdmPhaseCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        const bool evenFirst = !m_tdmPhaseCombo || m_tdmPhaseCombo->currentData().toInt() == 0;
        if (m_hub && m_hub->tdmContext()) {
            m_hub->tdmContext()->setPair02(evenFirst);
            return;
        }
        applyTdmState(m_tdmCheckbox && m_tdmCheckbox->isChecked(), evenFirst);
    });

    m_plotModeStack = new QStackedWidget;

    auto *gridW = new QWidget;
    gridW->setProperty("role", "plot-area");
    auto *grid = new QGridLayout(gridW);
    grid->setContentsMargins(10, 10, 10, 10);
    grid->setSpacing(8);

    m_waveWidgets.clear();
    for (int i = 0; i < 32; ++i) {
        auto *w = new WaveformWidget(i < defaultChannels.size() ? QStringLiteral("CH %1").arg(defaultChannels[i])
                                                                : QStringLiteral("CH --"));
        w->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("电压 (V)"));
        m_waveWidgets.push_back(w);
        grid->addWidget(w, i / 4, i % 4);
    }

    auto *stackW = new QWidget;
    stackW->setProperty("role", "plot-area");
    auto *stackLayout = new QHBoxLayout(stackW);
    stackLayout->setContentsMargins(10, 10, 10, 10);
    stackLayout->setSpacing(8);
    m_stackLeftView = new StackWaveformView(stackW);
    m_stackRightView = new StackWaveformView(stackW);
    m_stackLeftView->setPlaceholderText(QStringLiteral("Stack view\nleft 16 traces"));
    m_stackRightView->setPlaceholderText(QStringLiteral("Stack view\nright 16 traces"));
    stackLayout->addWidget(m_stackLeftView, 1);
    stackLayout->addWidget(m_stackRightView, 1);

    m_plotModeStack->addWidget(gridW);
    m_plotModeStack->addWidget(stackW);
    m_plotModeStack->setCurrentIndex(m_defaultViewMode == 1 ? 1 : 0);

    // ---- Heatmap in the left panel, below the controls ----
    auto *hmHeader = new QHBoxLayout;
    auto *heatmapTitle = new QLabel(QStringLiteral("全通道状态"));
    heatmapTitle->setProperty("role", "field-label");
    hmHeader->addWidget(heatmapTitle);
    hmHeader->addStretch(1);
    m_heatmapModeCombo = new QComboBox;
    m_heatmapModeCombo->addItem(QStringLiteral("AC RMS"), ActivityMapView::MetricRms);
    m_heatmapModeCombo->addItem(QStringLiteral("p2p"), ActivityMapView::MetricP2p);
    m_heatmapModeCombo->addItem(QStringLiteral("mean"), ActivityMapView::MetricMean);
    m_heatmapModeCombo->addItem(QStringLiteral("saturation"), ActivityMapView::MetricSaturation);
    setComboCurrentData(m_heatmapModeCombo, m_defaultHeatmapMetric);
    hmHeader->addWidget(m_heatmapModeCombo);
    sideCol->addLayout(hmHeader);

    m_activityMap = new ActivityMapView;
    m_activityMap->setMinimumHeight(220);
    m_activityMap->setMetricMode(m_heatmapModeCombo->currentData().toInt());
    m_activityMap->setTdmDisplay(m_tdmCheckbox && m_tdmCheckbox->isChecked(),
                                 !m_tdmPhaseCombo || m_tdmPhaseCombo->currentData().toInt() == 0);
    m_activitySummary = new QLabel(QStringLiteral("等待数据"));
    m_activitySummary->setProperty("role", "caption");
    m_activitySummary->setWordWrap(true);
    m_activitySummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sideCol->addWidget(m_activityMap, 1);
    sideCol->addWidget(m_activitySummary);
    connect(m_heatmapModeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_activityMap) {
            m_activityMap->setMetricMode(m_heatmapModeCombo->currentData().toInt());
        }
        scheduleSaveRealtimeConfig();
    });
    connect(m_activityMap, &ActivityMapView::channelClicked,
            this, &RealtimePlotPanel::selectHeatmapWindow);

    updateStackYScale();

    // ---- Assemble: left panel (controls + heatmap) | plot stack ----
    auto *mainRow = new QHBoxLayout;
    mainRow->setSpacing(10);
    mainRow->addWidget(side);
    mainRow->addWidget(m_plotModeStack, 1);
    root->addLayout(mainRow, 1);
}

QVector<int> RealtimePlotPanel::parseChannels() const {
    QVector<int> chs;
    const QStringList parts = m_rtChEdit->text().split(',', Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        bool ok = false;
        const int channel = p.trimmed().toInt(&ok);
        if (!ok) {
            throw std::runtime_error("通道必须是0-255之间的整数");
        }
        chs.push_back(channel);
    }

    if (chs.size() < 1 || chs.size() > 32) {
        throw std::runtime_error("需要选择1-32个通道");
    }

    QSet<int> unique;
    for (int ch : chs) {
        if (ch < 0 || ch >= kChannelsTotal) {
            throw std::runtime_error("通道范围必须是0-255");
        }
        unique.insert(ch);
    }
    if (unique.size() != chs.size()) {
        throw std::runtime_error("通道不能重复");
    }
    return chs;
}

bool RealtimePlotPanel::stackMode() const
{
    return m_viewModeCombo && m_viewModeCombo->currentData().toInt() == 1;
}

int RealtimePlotPanel::displaySignalMode() const
{
    return m_signalModeCombo ? m_signalModeCombo->currentData().toInt() : DisplayDc;
}

int RealtimePlotPanel::stackYScaleMode() const
{
    return m_stackYScaleCombo
               ? m_stackYScaleCombo->currentData().toInt()
               : m_defaultStackYScaleMode;
}

void RealtimePlotPanel::updateStackYScale()
{
    const auto mode = stackYScaleMode() == StackAdaptiveY
                          ? StackWaveformView::AdaptiveYScale
                          : StackWaveformView::FixedYScale;
    const bool dc = displaySignalMode() == DisplayDc;
    const double minimum = dc ? 0.0 : -0.1;
    const double maximum = dc ? 1.8 : 0.1;
    for (StackWaveformView *view : {m_stackLeftView, m_stackRightView}) {
        if (!view) {
            continue;
        }
        view->setYScaleMode(mode);
        view->setFixedYRange(minimum, maximum);
    }
}

QVector<int> RealtimePlotPanel::effectiveChannelsForMode(const QVector<int> &channels) const
{
    const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
    if (tdmMode && channels.size() > 16) {
        return channels.mid(0, 16);
    }
    return channels;
}

void RealtimePlotPanel::applyChannels() {
    if (applyChannelsInternal()) {
        scheduleSaveRealtimeConfig();
        if (m_streaming) {
            startStream();
        }
    }
}

bool RealtimePlotPanel::applyChannelsInternal() {
    QVector<int> chs;
    try {
        chs = parseChannels();
    } catch (const std::exception &exc) {
        QMessageBox::warning(this, QStringLiteral("通道错误"), QString::fromUtf8(exc.what()));
        return false;
    }
    const QVector<int> effectiveChannels = effectiveChannelsForMode(chs);
    {
        const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
        const int rawSamplesPerView = tdmMode ? m_samplesPerView * kTdmPhaseCount : m_samplesPerView;
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->channels = effectiveChannels;
        m_streamState->maxSamples = rawSamplesPerView;
        m_streamState->buffers.clear();
        m_streamState->totalSamples.clear();
        const qint64 initialFrameIndex = m_hub ? m_hub->currentFrameIndex() : 0;
        for (int ch : effectiveChannels) {
            m_streamState->buffers[ch] = QQueue<double>();
            m_streamState->totalSamples[ch] = initialFrameIndex;
        }
    }

    for (int i = 0; i < effectiveChannels.size(); ++i) {
        m_waveWidgets[i]->setTitle(QStringLiteral("CH %1").arg(effectiveChannels[i]));
        m_waveWidgets[i]->show();
    }
    for (int i = effectiveChannels.size(); i < m_waveWidgets.size(); ++i) {
        m_waveWidgets[i]->setData({});
        m_waveWidgets[i]->hide();
    }
    if (m_stackLeftView) {
        m_stackLeftView->setTraces({});
    }
    if (m_stackRightView) {
        m_stackRightView->setTraces({});
    }
    if (m_activityMap) {
        m_activityMap->setSelectedChannels(effectiveChannels);
        const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
        const bool evenSampleIsFirstLocalEle = !m_tdmPhaseCombo || m_tdmPhaseCombo->currentData().toInt() == 0;
        m_activityMap->setTdmDisplay(tdmMode, evenSampleIsFirstLocalEle);
    }

    const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
    const int outputCount = tdmMode ? effectiveChannels.size() * 2 : effectiveChannels.size();
    const bool truncated = effectiveChannels.size() != chs.size();
    setStatusLine(m_rtStatus,
                  QStringLiteral("状态: 通道已更新 | %1%2 | 输出 %3 条")
                      .arg(tdmMode ? QStringLiteral("TDM ADC ") : QStringLiteral("CH "))
                      .arg(effectiveChannels.size())
                      .arg(outputCount)
                      + (truncated ? QStringLiteral(" | 已取前16个ADC") : QString()),
                  truncated ? "warn" : "ok");
    return true;
}

void RealtimePlotPanel::selectHeatmapWindow(int centerChannel)
{
    if (centerChannel < 0 || centerChannel >= kChannelsTotal) {
        return;
    }
    const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
    const int count = tdmMode ? 16 : 32;
    const int maxStart = qMax(0, kChannelsTotal - count);
    const int start = qBound(0, centerChannel - count / 2, maxStart);

    QStringList channels;
    channels.reserve(count);
    for (int i = 0; i < count; ++i) {
        channels << QString::number(start + i);
    }
    m_rtChEdit->setText(channels.join(','));
    if (applyChannelsInternal()) {
        scheduleSaveRealtimeConfig();
        setStatusLine(m_rtStatus,
                      QStringLiteral("状态: Heatmap CH%1 -> 已载入 %2%3")
                          .arg(centerChannel)
                          .arg(tdmMode ? QStringLiteral("16 ADC / 32 TDM输出") : QStringLiteral("32 CH"))
                          .arg(tdmMode ? QStringLiteral("") : QStringLiteral("")),
                      "ok");
        refresh();
    }
}

void RealtimePlotPanel::applyViewLength() {
    if (applyViewLengthInternal(true)) {
        scheduleSaveRealtimeConfig();
    }
}

bool RealtimePlotPanel::applyViewLengthInternal(bool showMessage) {
    if (!m_rtPointsSpin) {
        return true;
    }

    m_samplesPerView = qMax(128, m_rtPointsSpin->value());
    const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
    const int rawSamplesPerView = tdmMode ? m_samplesPerView * kTdmPhaseCount : m_samplesPerView;
    {
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->maxSamples = rawSamplesPerView;
        for (int ch : m_streamState->channels) {
            QQueue<double> old = m_streamState->buffers.value(ch);
            while (old.size() > rawSamplesPerView) {
                old.dequeue();
            }
            m_streamState->buffers[ch] = old;
        }
    }

    const double effectiveSamplingRate = tdmMode ? m_samplingRate / kTdmPhaseCount : m_samplingRate;
    const double seconds = static_cast<double>(m_samplesPerView - 1) / qMax(1.0, effectiveSamplingRate);
    for (WaveformWidget *w : m_waveWidgets) {
        w->setXRange(0.0, seconds);
        w->setYRange(0.0, 1.8);
    }
    if (m_stackLeftView) {
        m_stackLeftView->setTimeSpanSeconds(seconds);
    }
    if (m_stackRightView) {
        m_stackRightView->setTimeSpanSeconds(seconds);
    }

    if (m_refreshTimer.isActive()) {
        m_refreshTimer.start(refreshIntervalMs());
    }
    updateTimeHint();
    if (showMessage && m_rtStatus) {
        setStatusLine(m_rtStatus, QStringLiteral("状态: X轴长度已更新"), "ok");
    }
    return true;
}

int RealtimePlotPanel::refreshIntervalMs() const {
    const int hz = m_refreshHzSpin ? qMax(1, m_refreshHzSpin->value()) : 10;
    int effectiveHz = hz;
    int channelCount = 32;
    if (m_streamState) {
        QMutexLocker locker(&m_streamState->lock);
        channelCount = m_streamState->channels.size();
    }
    const int visibleOutputs = m_tdmCheckbox && m_tdmCheckbox->isChecked()
                                   ? qMin(32, channelCount * 2)
                                   : qMin(32, channelCount);
    const qint64 totalPoints = static_cast<qint64>(qMax(1, visibleOutputs)) * qMax(1, m_samplesPerView);
    if (totalPoints > 2'000'000) {
        effectiveHz = qMin(effectiveHz, 1);
    } else if (totalPoints > 500'000) {
        effectiveHz = qMin(effectiveHz, 2);
    } else if (totalPoints > 100'000) {
        effectiveHz = qMin(effectiveHz, 10);
    }
    return qMax(16, 1000 / qMax(1, effectiveHz));
}

void RealtimePlotPanel::updateTimeHint() {
    if (!m_rtTimeHint) {
        return;
    }
    const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
    const double effectiveSamplingRate = tdmMode ? m_samplingRate / kTdmPhaseCount : m_samplingRate;
    const double timeMs = static_cast<double>(m_samplesPerView) / qMax(1.0, effectiveSamplingRate) * 1000.0;
    const double refreshHz = 1000.0 / static_cast<double>(qMax(1, refreshIntervalMs()));
    m_rtTimeHint->setText(QStringLiteral("X: %1 ms · %2 Hz · 折线滚动 · %3 · %4%5")
                              .arg(timeMs, 0, 'f', 2)
                              .arg(refreshHz, 0, 'f', 1)
                              .arg(displayModeSuffix(displaySignalMode()))
                              .arg(stackYScaleMode() == StackAdaptiveY ? QStringLiteral("自适应Y")
                                                                       : QStringLiteral("固定Y"))
                              .arg(tdmMode ? QStringLiteral(", fs/4") : QString()));
}

void RealtimePlotPanel::saveRealtimeConfig() const
{
    if (!m_cfgMgr) {
        return;
    }

    ConfigMap cfg = m_cfgMgr->load();
    ConfigSection &realtime = cfg[QStringLiteral("Realtime")];
    realtime[QStringLiteral("command")] = m_rtCmd ? m_rtCmd->text().trimmed() : m_defaultCommand;
    realtime[QStringLiteral("channels")] = m_rtChEdit ? channelsToText(channelsFromText(m_rtChEdit->text(), 32))
                                                      : m_defaultChannelsText;
    if (realtime.value(QStringLiteral("channels")).isEmpty() && m_rtChEdit) {
        realtime[QStringLiteral("channels")] = m_rtChEdit->text().trimmed();
    }
    realtime[QStringLiteral("wave_points")] = QString::number(m_rtPointsSpin ? m_rtPointsSpin->value() : m_samplesPerView);
    realtime[QStringLiteral("refresh_hz")] = QString::number(m_refreshHzSpin ? m_refreshHzSpin->value() : m_defaultRefreshHz);
    realtime[QStringLiteral("view_mode")] =
        viewModeToConfig(m_viewModeCombo ? m_viewModeCombo->currentData().toInt() : m_defaultViewMode);
    realtime[QStringLiteral("signal_mode")] =
        signalModeToConfig(m_signalModeCombo ? m_signalModeCombo->currentData().toInt() : m_defaultSignalMode);
    realtime[QStringLiteral("reference_mode")] =
        QString::number(m_refModeCombo ? m_refModeCombo->currentData().toInt() : m_defaultRefMode);
    realtime.remove(QStringLiteral("render_mode"));
    realtime.remove(QStringLiteral("refresh_mode"));
    // TDM moved to the global [Session] section; drop the stale page keys.
    realtime.remove(QStringLiteral("tdm_enabled"));
    realtime.remove(QStringLiteral("tdm_phase"));
    realtime[QStringLiteral("stack_y_mode")] = stackYScaleModeToConfig(stackYScaleMode());
    realtime[QStringLiteral("heatmap_metric")] =
        heatmapMetricToConfig(m_heatmapModeCombo ? m_heatmapModeCombo->currentData().toInt() : m_defaultHeatmapMetric);
    m_cfgMgr->save(cfg);
}

void RealtimePlotPanel::startStream() {
    m_streaming = false;
    m_paused = false;
    if (m_refreshTimer.isActive()) {
        m_refreshTimer.stop();
    }
    if (!stopThreads()) {
        setStatusLine(m_rtStatus, QStringLiteral("错误: 上一次数据流仍在停止中，请稍后重试"), "error");
        return;
    }
    if (m_hub) {
        m_samplingRate = m_hub->sampleRate();
    }
    if (!applyViewLengthInternal(false)) {
        return;
    }
    if (!applyChannelsInternal()) {
        return;
    }
    // NOTE: no saveRealtimeConfig() here — every user-facing edit handler
    // already persists, and startStream runs on page switches and replay
    // seeks where a synchronous config write would just add jank.

    const QString host = m_networkState->host();
    const int controlPort = m_networkState->port();  // typically 7
    const int dataPort = m_networkState->dataPort();
    const QString cmd = m_rtCmd->text().trimmed();

    m_stopFlag = std::make_unique<std::atomic_bool>(false);
    m_rawQueue->clear();
    m_rxBytes = 0;
    m_parsedFrames = 0;
    m_droppedFrames = 0;
    m_leftoverBytes = 0;
    m_lastBufferSamples = 0;
    // V6: the global session is started from the toolbar; the page subscribes
    // to whatever it serves (live or replay).
    qint64 initialFrameIndex = 0;
    if (m_hub) {
        initialFrameIndex = m_hub->addSubscriberWithFrameOrigin(m_rawQueue);
    }
    {
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->droppedFrames = 0;
        m_streamState->timelineEpoch = m_hub ? m_hub->timelineEpoch() : 0;
        m_streamState->metricWindowFrames =
            qMax(64, static_cast<int>(std::llround(m_samplingRate * 0.1)));
        for (int ch : m_streamState->channels) {
            m_streamState->totalSamples[ch] = initialFrameIndex;
        }
    }

    m_sorter = new DataSorter(m_rawQueue,
                              m_streamState,
                              m_stopFlag.get(),
                              m_hub ? m_hub->timelineEpochCounter() : nullptr,
                              m_hub ? m_hub->referenceModeCounter() : nullptr,
                              m_hub ? m_hub->timelineFrameOriginCounter() : nullptr,
                              initialFrameIndex,
                              this);
    DataSorter *const activeSorter = m_sorter;
    m_firstRefreshPending = true;
    connect(m_sorter, &DataSorter::framesParsed, this,
            [this, activeSorter](int frameCount, int leftoverBytes) {
        if (m_sorter != activeSorter) return;
        m_parsedFrames += frameCount;
        m_leftoverBytes = leftoverBytes;
        if (m_firstRefreshPending && m_streaming) {
            m_firstRefreshPending = false;
            QTimer::singleShot(0, this, &RealtimePlotPanel::refresh);
        }
    });
    m_sorter->start();

    m_paused = false;
    m_streaming = true;
    m_refreshTimer.start(refreshIntervalMs());
    const QString mode = controlPort == dataPort ? QStringLiteral("单端口") : QStringLiteral("双端口");
    setStatusLine(m_rtStatus,
                  QStringLiteral("状态: 实时采集中 %1 (%2 控制:%3 数据:%4)")
                      .arg(host)
                      .arg(mode)
                      .arg(controlPort)
                      .arg(dataPort),
                  "ok");
}

void RealtimePlotPanel::stopStream() {
    m_streaming = false;
    m_paused = false;
    m_firstRefreshPending = false;
    if (m_refreshTimer.isActive()) {
        m_refreshTimer.stop();
    }
    if (stopThreads()) {
        setStatusLine(m_rtStatus, QStringLiteral("状态: 已停止"), "warn");
    } else {
        setStatusLine(m_rtStatus, QStringLiteral("状态: 正在停止，请稍候"), "warn");
    }
}

bool RealtimePlotPanel::stopThreads() {
    if (m_hub) {
        m_hub->removeSubscriber(m_rawQueue);
    }
    if (m_stopFlag) {
        m_stopFlag->store(true);
    }
    if (m_rawQueue) {
        m_rawQueue->wakeAll();
    }

    bool allStopped = true;
    if (m_receiver) {
        if (!m_receiver->wait(250)) {
            m_receiver->requestInterruption();
            if (m_rawQueue) {
                m_rawQueue->wakeAll();
            }
            m_receiver->wait(100);
        }
        if (!m_receiver->isRunning()) {
            m_receiver->deleteLater();
            m_receiver = nullptr;
        } else {
            allStopped = false;
        }
    }
    if (m_sorter) {
        if (!m_sorter->wait(250)) {
            if (m_rawQueue) {
                m_rawQueue->wakeAll();
            }
            m_sorter->wait(100);
        }
        if (!m_sorter->isRunning()) {
            m_sorter->deleteLater();
            m_sorter = nullptr;
        } else {
            allStopped = false;
        }
    }
    return allStopped;
}

void RealtimePlotPanel::pauseStream() {
    m_paused = true;
    setStatusLine(m_rtStatus, QStringLiteral("状态: 已暂停"), "warn");
}

void RealtimePlotPanel::resumeStream() {
    m_paused = false;
    setStatusLine(m_rtStatus, QStringLiteral("状态: 实时采集中"), "ok");
}

void RealtimePlotPanel::updateOriginPlots(const QVector<int> &chs,
                                          const QVector<QVector<double>> &samples,
                                          const QVector<qint64> &firstSampleIndices,
                                          bool tdmMode,
                                          bool evenSampleIsFirstLocalEle)
{
    const int mode = displaySignalMode();
    const double effectiveSamplingRate = tdmMode ? m_samplingRate / kTdmPhaseCount : m_samplingRate;
    for (int i = 0; i < chs.size() && i < m_waveWidgets.size(); ++i) {
        if (tdmMode) {
            const QVector<TdmSeries> split =
                splitTdmSeries(chs[i], firstSampleIndices.value(i), samples.value(i), evenSampleIsFirstLocalEle);
            QVector<WaveformWidget::PlotSeries> series;
            series.reserve(split.size());
            double maxAbs = 0.0;
            for (const TdmSeries &part : split) {
                if (part.data.isEmpty()) {
                    continue;
                }
                WaveformWidget::PlotSeries s;
                s.data = transformDisplayData(part.data, mode, effectiveSamplingRate);
                for (double v : s.data) {
                    maxAbs = qMax(maxAbs, std::abs(v));
                }
                s.color = colorForTdmLocalEle(part.localEle);
                s.label = QStringLiteral("%1 %2").arg(part.label, displayModeSuffix(mode));
                series.push_back(s);
            }
            m_waveWidgets[i]->setTitle(QStringLiteral("CH %1 TDM").arg(chs[i]));
            if (mode == DisplayDc) {
                m_waveWidgets[i]->setYRange(0.0, 1.8);
            } else {
                const double range = qMax(1e-4, maxAbs * 1.15);
                m_waveWidgets[i]->setYRange(-range, range);
            }
            m_waveWidgets[i]->setSeries(series);
        } else {
            m_waveWidgets[i]->setTitle(QStringLiteral("CH %1").arg(chs[i]));
            const QVector<double> displayData = transformDisplayData(samples.value(i), mode, effectiveSamplingRate);
            if (mode == DisplayDc) {
                m_waveWidgets[i]->setYRange(0.0, 1.8);
            } else {
                double maxAbs = 0.0;
                for (double v : displayData) {
                    maxAbs = qMax(maxAbs, std::abs(v));
                }
                const double range = qMax(1e-4, maxAbs * 1.15);
                m_waveWidgets[i]->setYRange(-range, range);
            }
            m_waveWidgets[i]->setData(displayData);
        }
    }
}

void RealtimePlotPanel::updateStackPlots(const QVector<int> &chs,
                                         const QVector<QVector<double>> &samples,
                                         const QVector<qint64> &firstSampleIndices,
                                         bool tdmMode,
                                         bool evenSampleIsFirstLocalEle)
{
    QVector<StackWaveformView::Trace> traces;
    traces.reserve(tdmMode ? chs.size() * 2 : chs.size());
    const int mode = displaySignalMode();
    const double effectiveSamplingRate = tdmMode ? m_samplingRate / kTdmPhaseCount : m_samplingRate;

    if (tdmMode) {
        for (int i = 0; i < chs.size(); ++i) {
            const QVector<TdmSeries> split =
                splitTdmSeries(chs[i], firstSampleIndices.value(i), samples.value(i), evenSampleIsFirstLocalEle);
            for (const TdmSeries &part : split) {
                StackWaveformView::Trace trace;
                trace.data = transformDisplayData(part.data, mode, effectiveSamplingRate);
                trace.label = QStringLiteral("%1 %2").arg(part.label, displayModeSuffix(mode));
                trace.color = colorForTdmLocalEle(part.localEle);
                traces.push_back(trace);
            }
        }
    } else {
        for (int i = 0; i < chs.size(); ++i) {
            StackWaveformView::Trace trace;
            trace.data = transformDisplayData(samples.value(i), mode, effectiveSamplingRate);
            trace.label = QStringLiteral("CH %1 %2")
                              .arg(chs[i], 3, 10, QLatin1Char('0'))
                              .arg(displayModeSuffix(mode));
            trace.color = colorForStackIndex(i, chs.size());
            traces.push_back(trace);
        }
    }

    QVector<StackWaveformView::Trace> left;
    QVector<StackWaveformView::Trace> right;
    left.reserve(16);
    right.reserve(16);
    for (int i = 0; i < traces.size(); ++i) {
        if (i < 16) {
            left.push_back(traces[i]);
        } else if (i < 32) {
            right.push_back(traces[i]);
        }
    }

    if (m_stackLeftView) {
        m_stackLeftView->setTraces(left);
    }
    if (m_stackRightView) {
        m_stackRightView->setTraces(right);
    }
}

void RealtimePlotPanel::updateActivityMap(const QVector<int> &selectedChannels,
                                          const QVector<RealtimeChannelMetric> &metrics,
                                          const QVector<RealtimeChannelMetric> &tdmMetrics,
                                          bool tdmMode,
                                          bool evenSampleIsFirstLocalEle,
                                          quint64 metricsEpoch)
{
    if (!m_activityMap || metrics.isEmpty() || metricsEpoch == 0 || metricsEpoch == m_lastMetricsEpoch) {
        return;
    }

    m_lastMetricsEpoch = metricsEpoch;
    m_activityMap->setSelectedChannels(selectedChannels);
    m_activityMap->setTdmDisplay(tdmMode, evenSampleIsFirstLocalEle);
    m_activityMap->setMetrics(metrics);
    if (!tdmMetrics.isEmpty()) {
        m_activityMap->setTdmMetrics(tdmMetrics);
    }

    int saturated = 0;
    double maxRms = 0.0;
    double maxP2p = 0.0;
    const QVector<RealtimeChannelMetric> &activeMetrics = tdmMode && !tdmMetrics.isEmpty() ? tdmMetrics : metrics;
    for (const RealtimeChannelMetric &metric : activeMetrics) {
        if (!metric.valid) {
            continue;
        }
        if (metric.saturated) {
            ++saturated;
        }
        maxRms = qMax(maxRms, metric.rms);
        maxP2p = qMax(maxP2p, metric.p2p);
    }
    if (m_activitySummary) {
        m_activitySummary->setText(QStringLiteral("%1 Max AC RMS %2 · Max p2p %3 · Sat %4 · Sel %5")
                                       .arg(tdmMode ? QStringLiteral("TDM") : QStringLiteral("ADC"))
                                       .arg(metricVoltageText(maxRms))
                                       .arg(metricVoltageText(maxP2p))
                                       .arg(saturated)
                                       .arg(selectedChannels.size()));
    }
}

void RealtimePlotPanel::refresh() {
    if (!m_streaming) {
        return;
    }
    if (m_paused) {
        return;
    }
    if (m_hub) {
        const SessionHub::Statistics stats = m_hub->statistics();
        m_rxBytes = stats.receivedBytes;
        m_droppedFrames =
            stats.ingressDroppedFrames + stats.subscriberDroppedFrames;
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->droppedFrames = static_cast<quint64>(qMax<qint64>(0, m_droppedFrames));
    }

    const bool tdmMode = m_tdmCheckbox && m_tdmCheckbox->isChecked();
    const bool evenSampleIsFirstLocalEle = !m_tdmPhaseCombo || m_tdmPhaseCombo->currentData().toInt() == 0;
    const int rawSamplesPerView = tdmMode ? m_samplesPerView * kTdmPhaseCount : m_samplesPerView;
    QVector<int> chs;
    QVector<QVector<double>> samples;
    QVector<qint64> firstSampleIndices;
    QVector<RealtimeChannelMetric> metrics;
    QVector<RealtimeChannelMetric> tdmMetrics;
    quint64 metricsEpoch = 0;
    int maxBuffered = 0;
    {
        QMutexLocker locker(&m_streamState->lock);
        chs = m_streamState->channels;
        metrics = m_streamState->metrics;
        tdmMetrics = m_streamState->tdmMetrics;
        metricsEpoch = m_streamState->metricsEpoch;
        samples.reserve(chs.size());
        firstSampleIndices.reserve(chs.size());
        for (int ch : chs) {
            auto it = m_streamState->buffers.constFind(ch);
            if (it == m_streamState->buffers.cend()) {
                samples.push_back({});
                firstSampleIndices.push_back(0);
                continue;
            }
            const QQueue<double> &q = it.value();
            maxBuffered = qMax(maxBuffered, q.size());
            const int count = qMin(rawSamplesPerView, q.size());
            QVector<double> data;
            data.reserve(count);
            const int start = q.size() - count;
            for (int j = start; j < q.size(); ++j) {
                data.push_back(q.at(j));
            }
            samples.push_back(data);
            const qint64 totalSamples = m_streamState->totalSamples.value(ch, q.size());
            firstSampleIndices.push_back(qMax<qint64>(0, totalSamples - count));
        }
    }

    if (stackMode()) {
        updateStackPlots(chs, samples, firstSampleIndices, tdmMode, evenSampleIsFirstLocalEle);
    } else {
        updateOriginPlots(chs, samples, firstSampleIndices, tdmMode, evenSampleIsFirstLocalEle);
    }
    updateActivityMap(chs, metrics, tdmMetrics, tdmMode, evenSampleIsFirstLocalEle, metricsEpoch);
    m_lastBufferSamples = maxBuffered;
    setStatusLine(m_rtStatus,
                  QStringLiteral("状态: RX %1 B | frames %2 | dropped %3 | leftover %4 B | buffered %5%6")
                      .arg(m_rxBytes)
                      .arg(m_parsedFrames)
                      .arg(m_droppedFrames)
                      .arg(m_leftoverBytes)
                      .arg(m_lastBufferSamples)
                      .arg(tdmMode ? QStringLiteral(" | TDM") : QString()),
                  m_droppedFrames > 0 ? "warn" : "info");
}

void RealtimePlotPanel::onActivated() {
    if (m_hub && m_hub->isRunning() && !m_streaming) {
        startStream();
    }
}

void RealtimePlotPanel::onDeactivated() {
    if (m_streaming) {
        stopStream();
    }
}

void RealtimePlotPanel::shutdown() {
    if (m_saveDebounceTimer.isActive()) {
        m_saveDebounceTimer.stop();
        saveRealtimeConfig();
    }
    stopStream();
    // closeEvent() destroys the page right after this returns; a receiver or
    // sorter that outlived stopThreads()' bounded waits would then be a
    // running QThread destroyed with its stop flag freed underneath it. Join
    // unconditionally here (sweep/analyzer pages do the same).
    if (!stopThreads()) {
        if (m_receiver && m_receiver->isRunning()) {
            m_receiver->wait();
        }
        if (m_rawQueue) m_rawQueue->wakeAll();
        if (m_sorter && m_sorter->isRunning()) {
            m_sorter->wait();
        }
        stopThreads();
    }
}

void RealtimePlotPanel::setSessionHub(SessionHub *hub) {
    m_hub = hub;
    if (!m_hub) return;
    // Seed the global software reference from this page's persisted choice.
    if (m_refModeCombo) m_hub->setReferenceMode(m_refModeCombo->currentData().toInt());
    if (TdmContext *tdm = m_hub->tdmContext()) {
        applyTdmState(tdm->enabled(), tdm->pair02());
        connect(tdm, &TdmContext::changed, this, &RealtimePlotPanel::applyTdmState);
    }
    connect(m_hub, &SessionHub::connectionStateChanged, this, [this](bool connected) {
        if (!connected && m_streaming) {
            setStatusLine(m_rtStatus, QStringLiteral("状态: 采集连接已断开"), "warn");
        }
    });
    connect(m_hub, &SessionHub::connectionEvent, this, [this](const QString &msg) {
        if (m_streaming) setStatusLine(m_rtStatus, QStringLiteral("状态: %1").arg(msg), "ok");
    });
    connect(m_hub,
            &SessionHub::timelineReset,
            this,
            [this](quint64 epoch, qint64 targetFrame) {
        resetTimelineState(epoch, targetFrame);
    });
}

void RealtimePlotPanel::applyTdmState(bool enabled, bool evenFirst) {
    const bool enabledChanged = m_tdmCheckbox && m_tdmCheckbox->isChecked() != enabled;
    if (m_tdmCheckbox) {
        const QSignalBlocker blocker(m_tdmCheckbox);
        m_tdmCheckbox->setChecked(enabled);
    }
    if (m_tdmPhaseCombo) {
        const QSignalBlocker blocker(m_tdmPhaseCombo);
        setComboCurrentData(m_tdmPhaseCombo, evenFirst ? 0 : 1);
        m_tdmPhaseCombo->setEnabled(enabled);
    }
    if (m_activityMap) {
        m_activityMap->setTdmDisplay(enabled, evenFirst);
    }
    // Turning the interleave on/off changes trace count and view length;
    // a phase change only relabels — refresh() picks it up per frame.
    if (enabledChanged) {
        applyViewLengthInternal(false);
        if (applyChannelsInternal() && m_streaming) {
            startStream();
        }
    }
    refresh();
}

void RealtimePlotPanel::resetTimelineState(quint64 epoch, qint64 targetFrame)
{
    // Seek barrier: the DataSorter keeps running and drops stale chunks by
    // itself (epoch check), so a seek only needs the buffers/plots cleared —
    // no thread teardown, no GUI-blocking waits.
    if (m_rawQueue) m_rawQueue->clear();
    {
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->buffers.clear();
        m_streamState->totalSamples.clear();
        for (int ch : m_streamState->channels) {
            m_streamState->totalSamples[ch] = targetFrame;
        }
        m_streamState->metrics.clear();
        m_streamState->tdmMetrics.clear();
        m_streamState->timelineEpoch = epoch;
        ++m_streamState->metricsEpoch;
    }
    for (WaveformWidget *widget : m_waveWidgets) {
        if (widget) widget->setSeries({});
    }
    if (m_stackLeftView) m_stackLeftView->setTraces({});
    if (m_stackRightView) m_stackRightView->setTraces({});
    if (m_activityMap) m_activityMap->clearMetrics();
    m_parsedFrames = 0;
    m_leftoverBytes = 0;
    m_lastBufferSamples = 0;
    m_firstRefreshPending = m_streaming;
}

void RealtimePlotPanel::setWaveTheme(const QMap<QString, QString> &palette) {
    for (WaveformWidget *w : m_waveWidgets) {
        w->setPaletteColors(palette);
    }
    if (m_stackLeftView) {
        m_stackLeftView->setThemePalette(palette);
    }
    if (m_stackRightView) {
        m_stackRightView->setThemePalette(palette);
    }
    if (m_activityMap) {
        m_activityMap->setPaletteColors(palette);
    }
}

void RealtimePlotPanel::onNetworkChanged(const QString &host, int port) {
    if (m_streaming) {
        stopStream();
        setStatusLine(m_rtStatus, QStringLiteral("状态: 网络端点已更改，请重新开始采集"), "warn");
    }
    if (m_netLabel) {
        m_netLabel->setText(QStringLiteral("%1 控制:%2 数据:%3")
                                .arg(host)
                                .arg(port)
                                .arg(m_networkState->dataPort()));
    }
}

void RealtimePlotPanel::onGetChannelAddress() {
    QVector<int> channels = m_channelState->selectedGlobalChannels();
    if (channels.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("读取通道地址"), QStringLiteral("请先在1024通道分布页面选择通道"));
        return;
    }
    if (channels.size() > 32) {
        channels = channels.mid(0, 32);
        setStatusLine(m_rtStatus, QStringLiteral("状态: 已从通道图载入通道（截断到32个）"), "warn");
    } else {
        setStatusLine(m_rtStatus, QStringLiteral("状态: 已从通道图载入通道"), "ok");
    }

    QStringList txt;
    for (int ch : channels) {
        txt << QString::number(ch);
    }
    m_rtChEdit->setText(txt.join(','));
    if (applyChannelsInternal()) {
        scheduleSaveRealtimeConfig();
        if (m_streaming) startStream();
    }
}

}  // namespace ccv2
