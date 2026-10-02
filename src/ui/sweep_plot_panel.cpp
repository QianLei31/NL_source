#include "ui/sweep_plot_panel.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/network_state.h"
#include "core/channel_routing.h"
#include "core/tdm_context.h"
#include "config/config_manager.h"
#include "network/socket_receiver2.h"
#include "theme/theme_manager.h"
#include "ui/widgets/sweep_waveform_view.h"
#include "ui/widgets/activity_map_view.h"
#include "service/session_hub.h"

namespace ccv2 {

namespace {

void setComboData(QComboBox *combo, int value)
{
    if (!combo) return;
    const int index = combo->findData(value);
    if (index >= 0) combo->setCurrentIndex(index);
}

}  // namespace

SweepPlotPanel::SweepPlotPanel(ConfigManager *cfgMgr, NetworkState *networkState, QWidget *parent)
    : QWidget(parent),
      m_cfgMgr(cfgMgr),
      m_networkState(networkState),
      m_rawQueue(std::make_shared<ThreadSafeQueue<QByteArray>>(256)),
      m_streamState(std::make_shared<RealtimeStreamState>()),
      m_stopFlag(std::make_unique<std::atomic_bool>(false)) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &SweepPlotPanel::saveConfig);
    buildUi();
    loadConfig();
    connect(&m_refreshTimer, &QTimer::timeout, this, &SweepPlotPanel::refresh);
    if (m_networkState) {
        connect(m_networkState, &NetworkState::networkChanged, this,
                [this](const QString &, int, int) {
            if (m_streaming) {
                stopAcquisition();
                setStatus(QStringLiteral("状态: 网络端点已更改，请重新开始采集"), "warn");
            }
        });
    }
}

SweepPlotPanel::~SweepPlotPanel() {
    // By the time the page tree is destroyed the main window's ConfigManager
    // member is already gone; closeEvent()'s shutdown() has flushed any
    // pending save, so just make sure shutdown() can't trigger another one.
    m_saveTimer.stop();
    shutdown();
}

void SweepPlotPanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    auto fieldLabel = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setProperty("role", "field-label");
        return label;
    };

    // === Left: continuous sweep waveform ===
    m_view = new SweepWaveformView;
    m_view->setProperty("role", "plot-area");

    // === Right: vertical parameters (top) + all-channel heatmap (bottom) ===
    auto *side = new QFrame;
    side->setProperty("role", "page-control-panel");
    side->setMinimumWidth(280);
    side->setMaximumWidth(320);
    auto *sideCol = new QVBoxLayout(side);
    sideCol->setContentsMargins(0, 0, 0, 0);
    sideCol->setSpacing(6);

    auto *form = new QGridLayout;
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(5);
    form->setColumnStretch(1, 1);
    int r = 0;
    auto addField = [&](const QString &label, QWidget *w) {
        form->addWidget(fieldLabel(label), r, 0);
        form->addWidget(w, r, 1);
        ++r;
    };

    m_chEdit = new QLineEdit(QStringLiteral("0-7"));
    m_chEdit->setPlaceholderText(QStringLiteral("如 0-7,16,32-35"));
    addField(QStringLiteral("通道"), m_chEdit);

    m_cmdEdit = new QLineEdit(QStringLiteral("ctre"));
    m_cmdEdit->setPlaceholderText(QStringLiteral("如 ctre"));
    addField(QStringLiteral("命令"), m_cmdEdit);

    m_spanSpin = new QDoubleSpinBox;
    m_spanSpin->setRange(0.2, 30.0);
    m_spanSpin->setSingleStep(0.5);
    m_spanSpin->setValue(4.0);
    m_spanSpin->setSuffix(QStringLiteral(" s"));
    addField(QStringLiteral("时间窗"), m_spanSpin);

    m_fsSpin = new QSpinBox;
    m_fsSpin->setRange(2000, 500000);
    m_fsSpin->setValue(20000);
    m_fsSpin->setSuffix(QStringLiteral(" Hz"));
    m_fsSpin->setSingleStep(1000);
    addField(QStringLiteral("采样率"), m_fsSpin);

    m_yfsSpin = new QDoubleSpinBox;
    m_yfsSpin->setRange(0.01, 1.8);
    m_yfsSpin->setDecimals(2);
    m_yfsSpin->setSingleStep(0.1);
    m_yfsSpin->setValue(0.5);
    m_yfsSpin->setSuffix(QStringLiteral(" V"));
    addField(QStringLiteral("Y满量程"), m_yfsSpin);

    m_refreshSpin = new QSpinBox;
    m_refreshSpin->setRange(10, 60);
    m_refreshSpin->setValue(40);
    m_refreshSpin->setSuffix(QStringLiteral(" Hz"));
    addField(QStringLiteral("刷新"), m_refreshSpin);

    m_bandCombo = new QComboBox;
    m_bandCombo->addItem(QStringLiteral("宽带"), SpikeProcConfig::Wideband);
    m_bandCombo->addItem(QStringLiteral("Spike带"), SpikeProcConfig::SpikeBand);
    m_bandCombo->addItem(QStringLiteral("LFP带"), SpikeProcConfig::LfpBand);
    m_bandCombo->setCurrentIndex(1);
    addField(QStringLiteral("显示带"), m_bandCombo);

    m_notchCombo = new QComboBox;
    m_notchCombo->addItem(QStringLiteral("无"), 0);
    m_notchCombo->addItem(QStringLiteral("50 Hz"), 50);
    m_notchCombo->addItem(QStringLiteral("60 Hz"), 60);
    m_notchCombo->setCurrentIndex(1);
    addField(QStringLiteral("陷波"), m_notchCombo);

    m_hpSpin = new QSpinBox;
    m_hpSpin->setRange(50, 2000);
    m_hpSpin->setValue(250);
    m_hpSpin->setSuffix(QStringLiteral(" Hz"));
    m_hpSpin->setSingleStep(50);
    addField(QStringLiteral("高通"), m_hpSpin);

    m_spikeLpSpin = new QSpinBox;
    m_spikeLpSpin->setRange(300, 20000);
    m_spikeLpSpin->setValue(6000);
    m_spikeLpSpin->setSuffix(QStringLiteral(" Hz"));
    m_spikeLpSpin->setSingleStep(500);
    addField(QStringLiteral("Spike低通"), m_spikeLpSpin);

    m_orderCombo = new QComboBox;
    for (int o : {2, 4, 6, 8}) {
        m_orderCombo->addItem(QStringLiteral("%1 阶").arg(o), o);
    }
    m_orderCombo->setCurrentIndex(0);  // 2nd order (safe default; ringing-free)
    m_orderCombo->setToolTip(QStringLiteral(
        "带通滤波器阶数：阶数越高，频带边沿越陡；\n"
        "但高阶在尖锐暂态上会有振铃，可能造成 spike 双计数"));
    addField(QStringLiteral("滤波阶数"), m_orderCombo);

    m_threshModeCombo = new QComboBox;
    m_threshModeCombo->addItem(QStringLiteral("RMS 倍数"), 0);
    m_threshModeCombo->addItem(QStringLiteral("绝对 mV"), 1);
    addField(QStringLiteral("阈值模式"), m_threshModeCombo);

    m_threshSpin = new QDoubleSpinBox;
    m_threshSpin->setRange(1.0, 20.0);
    m_threshSpin->setValue(4.0);
    m_threshSpin->setDecimals(1);
    addField(QStringLiteral("阈值"), m_threshSpin);

    m_polarityCombo = new QComboBox;
    m_polarityCombo->addItem(QStringLiteral("负向"), true);
    m_polarityCombo->addItem(QStringLiteral("正向"), false);
    addField(QStringLiteral("极性"), m_polarityCombo);
    sideCol->addLayout(form);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    m_btnStart = new QPushButton(QStringLiteral("开始"));
    m_btnStart->setProperty("variant", "primary");
    m_btnStop = new QPushButton(QStringLiteral("停止"));
    m_btnStop->setProperty("variant", "danger");
    m_btnStop->setEnabled(false);
    m_btnPause = new QPushButton(QStringLiteral("暂停"));
    m_btnPause->setProperty("variant", "secondary");
    m_btnPause->setEnabled(false);
    btnRow->addWidget(m_btnStart);
    btnRow->addWidget(m_btnStop);
    btnRow->addWidget(m_btnPause);
    // V6: the session is run from the global toolbar; this page is a pure view.
    m_btnStart->setVisible(false);
    m_btnStop->setVisible(false);
    m_btnPause->setVisible(false);
    sideCol->addLayout(btnRow);

    // Raw all-channel status heatmap (RMS / P2P) below the parameters. The
    // sweep filters are only applied to the selected plotted channels.
    auto *mapHeader = new QHBoxLayout;
    mapHeader->addWidget(fieldLabel(QStringLiteral("原始 ADC 全通道状态")));
    mapHeader->addStretch(1);
    m_metricCombo = new QComboBox;
    m_metricCombo->addItem(QStringLiteral("RMS"), ActivityMapView::MetricRms);
    m_metricCombo->addItem(QStringLiteral("P2P"), ActivityMapView::MetricP2p);
    m_metricCombo->addItem(QStringLiteral("均值"), ActivityMapView::MetricMean);
    mapHeader->addWidget(m_metricCombo);
    sideCol->addLayout(mapHeader);

    auto *clickRow = new QHBoxLayout;
    clickRow->addWidget(fieldLabel(QStringLiteral("点击")));
    m_clickModeCombo = new QComboBox;
    m_clickModeCombo->addItem(QStringLiteral("窗口"), 0);
    m_clickModeCombo->addItem(QStringLiteral("单通道"), 1);
    clickRow->addWidget(m_clickModeCombo);
    clickRow->addStretch(1);
    sideCol->addLayout(clickRow);

    // Global TDM state: each ADC channel is split into its two interleaved
    // electrodes, filtered separately at fs/2. Bound to the shared TdmContext
    // so it tracks pages 2/3 and replay restores.
    m_tdmCheck = new QCheckBox(QStringLiteral("TDM分组显示"));
    m_tdmCheck->setToolTip(QStringLiteral(
        "全局 TDM 状态：与实时波形/分析器页联动。开启后每个 ADC 通道\n"
        "按帧奇偶拆成两个电极，各自以 fs/2 独立滤波与 spike 检测。"));
    m_tdmPhaseCombo = new QComboBox;
    m_tdmPhaseCombo->addItem(QStringLiteral("显示相位 0 / 2"), 0);
    m_tdmPhaseCombo->addItem(QStringLiteral("显示相位 1 / 3"), 1);
    m_tdmPhaseCombo->setEnabled(false);
    auto *tdmRow = new QHBoxLayout;
    tdmRow->addWidget(m_tdmCheck);
    tdmRow->addWidget(m_tdmPhaseCombo, 1);
    sideCol->addLayout(tdmRow);

    m_activityMap = new ActivityMapView;
    m_activityMap->setMinimumHeight(240);
    sideCol->addWidget(m_activityMap, 1);

    // === Assemble: main row (side | waveform) + status line ===
    auto *mainRow = new QHBoxLayout;
    mainRow->setSpacing(10);
    mainRow->addWidget(side);
    mainRow->addWidget(m_view, 1);
    root->addLayout(mainRow, 1);

    m_status = new QLabel(QStringLiteral("状态: 空闲"));
    m_status->setProperty("role", "status-line");
    m_status->setProperty("state", "info");
    root->addWidget(m_status);

    connect(m_btnStart, &QPushButton::clicked, this, &SweepPlotPanel::startStream);
    connect(m_btnStop, &QPushButton::clicked, this, &SweepPlotPanel::stopStream);
    connect(m_btnPause, &QPushButton::clicked, this, &SweepPlotPanel::togglePause);
    connect(m_chEdit, &QLineEdit::textChanged, this, [this](const QString &) { scheduleSaveConfig(); });
    connect(m_chEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_streaming) return;  // live channel apply (pure-view model has no start button)
        const QVector<int> chs = parseChannels(m_chEdit->text());
        if (!chs.isEmpty() && chs.size() <= 32) setDisplayChannels(chs);
    });
    connect(m_cmdEdit, &QLineEdit::textChanged, this, [this](const QString &) { scheduleSaveConfig(); });
    connect(m_spanSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        applyViewParams();
        scheduleSaveConfig();
    });
    connect(m_yfsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        applyViewParams();
        scheduleSaveConfig();
    });
    connect(m_fsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        updateFrequencyLimits();
        applyViewParams();
        applyProcConfig();
        if (m_view) m_view->resetSweep();
        scheduleSaveConfig();
    });
    connect(m_refreshSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int hz) {
        if (m_refreshTimer.isActive()) m_refreshTimer.start(qMax(16, 1000 / qMax(1, hz)));
        scheduleSaveConfig();
    });

    connect(m_threshModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const bool absolute = m_threshModeCombo->currentData().toInt() == 1;
        const QSignalBlocker blocker(m_threshSpin);
        if (absolute) {
            m_threshSpin->setRange(-2000.0, 2000.0);
            m_threshSpin->setSuffix(QStringLiteral(" mV"));
            m_threshSpin->setValue(-50.0);
        } else {
            m_threshSpin->setRange(1.0, 20.0);
            m_threshSpin->setSuffix(QString());
            m_threshSpin->setValue(4.0);
        }
        applyProcConfig();
        scheduleSaveConfig();
    });
    connect(m_bandCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        applyProcConfig();
        if (m_view) m_view->resetSweep();
        scheduleSaveConfig();
    });
    connect(m_notchCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        applyProcConfig();
        if (m_view) m_view->resetSweep();
        scheduleSaveConfig();
    });
    connect(m_polarityCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        applyProcConfig();
        scheduleSaveConfig();
    });
    connect(m_hpSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        updateFrequencyLimits();
        applyProcConfig();
        if (m_view) m_view->resetSweep();
        scheduleSaveConfig();
    });
    connect(m_spikeLpSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        updateFrequencyLimits();
        applyProcConfig();
        if (m_view) m_view->resetSweep();
        scheduleSaveConfig();
    });
    connect(m_orderCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        applyProcConfig();
        if (m_view) m_view->resetSweep();
        scheduleSaveConfig();
    });
    connect(m_threshSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        applyProcConfig();
        scheduleSaveConfig();
    });

    connect(m_metricCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_activityMap) m_activityMap->setMetricMode(m_metricCombo->currentData().toInt());
        scheduleSaveConfig();
    });
    connect(m_clickModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { scheduleSaveConfig(); });
    // Widgets mirror the global TdmContext; the actual page reconfigure happens
    // in applyTdmState, driven by the context's changed() signal (so a change
    // from another page or a replay restore updates this page too).
    connect(m_tdmCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_hub && m_hub->tdmContext()) {
            m_hub->tdmContext()->setEnabled(checked);
        } else {
            applyTdmState(checked, m_tdmEvenFirst);
        }
    });
    connect(m_tdmPhaseCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const bool evenFirst = m_tdmPhaseCombo->currentData().toInt() == 0;
        if (m_hub && m_hub->tdmContext()) {
            m_hub->tdmContext()->setPair02(evenFirst);
        } else {
            applyTdmState(m_tdmEnabled, evenFirst);
        }
    });
    connect(m_activityMap, &ActivityMapView::channelClicked, this, [this](int ch) {
        if (ch < 0 || ch >= 256) return;
        QVector<int> chs;
        if (m_clickModeCombo && m_clickModeCombo->currentData().toInt() == 1) {
            // Single-channel mode: toggle the clicked channel in/out of the set.
            chs = m_channels;
            const int idx = chs.indexOf(ch);
            if (idx >= 0) {
                if (chs.size() > 1) chs.remove(idx);  // keep at least one
            } else if (chs.size() < 32) {
                chs.push_back(ch);
                std::sort(chs.begin(), chs.end());
            }
        } else {
            // Window mode: a window of channels centered on the click (like page 2).
            const int count = m_channels.isEmpty() ? 8 : qBound(1, m_channels.size(), 32);
            const int maxStart = qMax(0, 256 - count);
            const int start = qBound(0, ch - count / 2, maxStart);
            for (int i = 0; i < count; ++i) chs.push_back(start + i);
        }
        QStringList txt;
        for (int c : chs) txt << QString::number(c);
        m_chEdit->setText(txt.join(QLatin1Char(',')));
        setDisplayChannels(chs);
    });
}

void SweepPlotPanel::setDisplayChannels(const QVector<int> &chs) {
    if (chs.isEmpty() || !m_view) return;
    m_channels = chs;
    m_view->setChannels(displayLanes());
    m_view->setSeriesColors(ThemeManager::seriesPalette(laneCount()));
    applyViewParams();
    applyProcConfig();
    m_view->resetSweep();
    m_lastTotal.clear();
    {
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->channels = chs;     // DataSorter picks this up live (raw ADC channels)
        m_streamState->buffers.clear();
        m_streamState->totalSamples.clear();
    }
    if (m_activityMap) m_activityMap->setSelectedChannels(chs);
    scheduleSaveConfig();
}

int SweepPlotPanel::laneCount() const {
    return m_channels.size() * (m_tdmEnabled ? 2 : 1);
}

double SweepPlotPanel::procSampleRate() const {
    const double fs = m_fsSpin ? static_cast<double>(m_fsSpin->value()) : 20000.0;
    return m_tdmEnabled ? fs / kTdmPhaseCount : fs;
}

QVector<int> SweepPlotPanel::displayLanes() const {
    if (!m_tdmEnabled) {
        return m_channels;
    }
    QVector<int> lanes;
    lanes.reserve(m_channels.size() * 2);
    for (int ch : m_channels) {
        const QPair<int, int> pair = tdmLocalElePair(m_tdmEvenFirst);
        lanes.push_back(globalEleForChannelLocalEle(ch, pair.first));
        lanes.push_back(globalEleForChannelLocalEle(ch, pair.second));
    }
    return lanes;
}

void SweepPlotPanel::applyProcConfig() {
    SpikeProcConfig cfg;
    // Each electrode is decimated to fs/4 in TDM mode, so the filter cutoffs
    // must be designed for that rate.
    cfg.sampleRate = procSampleRate();
    cfg.band = m_bandCombo->currentData().toInt();
    cfg.notchHz = m_notchCombo->currentData().toInt();
    cfg.highpassHz = static_cast<double>(m_hpSpin->value());
    cfg.spikeLowpassHz = static_cast<double>(m_spikeLpSpin->value());
    cfg.filterOrder = m_orderCombo->currentData().toInt();
    cfg.lowpassHz = 300.0;
    cfg.negativePolarity = m_polarityCombo->currentData().toBool();
    const bool absolute = m_threshModeCombo->currentData().toInt() == 1;
    cfg.absoluteThreshold = absolute;
    if (absolute) {
        cfg.absThresholdV = (cfg.negativePolarity ? -1.0 : 1.0)
                            * std::abs(m_threshSpin->value()) / 1000.0;  // mV -> V
    } else {
        cfg.rmsMultiple = m_threshSpin->value();
    }
    if (!m_channels.isEmpty()) {
        m_proc.configure(laneCount(), cfg);
    }
    if (m_view) {
        m_view->setVoltageMid(cfg.band == SpikeProcConfig::Wideband ? 0.9 : 0.0);
    }
}

QVector<int> SweepPlotPanel::parseChannels(const QString &text) const {
    QVector<int> out;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &partRaw : parts) {
        const QString part = partRaw.trimmed();
        if (part.isEmpty()) continue;
        const int dash = part.indexOf(QLatin1Char('-'));
        if (dash > 0) {
            bool ok1 = false, ok2 = false;
            const int a = part.left(dash).trimmed().toInt(&ok1);
            const int b = part.mid(dash + 1).trimmed().toInt(&ok2);
            if (ok1 && ok2) {
                for (int v = qMin(a, b); v <= qMax(a, b); ++v) {
                    if (v >= 0 && v < 256 && !out.contains(v)) out.push_back(v);
                }
            }
        } else {
            bool ok = false;
            const int v = part.toInt(&ok);
            if (ok && v >= 0 && v < 256 && !out.contains(v)) out.push_back(v);
        }
    }
    return out;
}

void SweepPlotPanel::applyViewParams() {
    if (!m_view) return;
    m_view->setSampleRate(procSampleRate());
    m_view->setTimeSpanSeconds(m_spanSpin->value());
    const bool wideband =
        m_bandCombo &&
        m_bandCombo->currentData().toInt() == SpikeProcConfig::Wideband;
    m_view->setVoltageMid(wideband ? 0.9 : 0.0);
    m_view->setYFullScaleVolts(m_yfsSpin->value());
}

void SweepPlotPanel::updateFrequencyLimits()
{
    if (!m_fsSpin || !m_hpSpin || !m_spikeLpSpin) return;

    const int upperLimit =
        qMax(300, static_cast<int>(std::floor(m_fsSpin->value() * 0.45)));
    const QSignalBlocker hpBlocker(m_hpSpin);
    const QSignalBlocker lpBlocker(m_spikeLpSpin);

    m_hpSpin->setMaximum(qMax(50, upperLimit - 50));
    m_spikeLpSpin->setMaximum(upperLimit);
    if (m_hpSpin->value() >= m_spikeLpSpin->value()) {
        if (m_hpSpin->value() + 50 <= upperLimit) {
            m_spikeLpSpin->setValue(m_hpSpin->value() + 50);
        } else {
            m_hpSpin->setValue(qMax(50, upperLimit - 50));
            m_spikeLpSpin->setValue(upperLimit);
        }
    }
}

void SweepPlotPanel::loadConfig()
{
    if (!m_cfgMgr) return;

    m_loadingConfig = true;
    const ConfigSection sweep =
        m_cfgMgr->load().value(QStringLiteral("Sweep"));
    const QSignalBlocker chBlocker(m_chEdit);
    const QSignalBlocker cmdBlocker(m_cmdEdit);
    const QSignalBlocker spanBlocker(m_spanSpin);
    const QSignalBlocker fsBlocker(m_fsSpin);
    const QSignalBlocker yBlocker(m_yfsSpin);
    const QSignalBlocker refreshBlocker(m_refreshSpin);
    const QSignalBlocker bandBlocker(m_bandCombo);
    const QSignalBlocker notchBlocker(m_notchCombo);
    const QSignalBlocker hpBlocker(m_hpSpin);
    const QSignalBlocker spikeLpBlocker(m_spikeLpSpin);
    const QSignalBlocker thresholdModeBlocker(m_threshModeCombo);
    const QSignalBlocker thresholdBlocker(m_threshSpin);
    const QSignalBlocker polarityBlocker(m_polarityCombo);
    const QSignalBlocker metricBlocker(m_metricCombo);
    const QSignalBlocker clickBlocker(m_clickModeCombo);

    m_chEdit->setText(sweep.value(QStringLiteral("channels"), QStringLiteral("0-7")));
    m_cmdEdit->setText(sweep.value(QStringLiteral("command"), QStringLiteral("ctre")));
    m_spanSpin->setValue(
        sweep.value(QStringLiteral("time_span"), QStringLiteral("4")).toDouble());
    m_fsSpin->setValue(
        sweep.value(QStringLiteral("sampling_rate"), QStringLiteral("20000")).toInt());
    m_yfsSpin->setValue(
        sweep.value(QStringLiteral("y_full_scale"), QStringLiteral("0.5")).toDouble());
    m_refreshSpin->setValue(
        sweep.value(QStringLiteral("refresh_hz"), QStringLiteral("40")).toInt());
    setComboData(m_bandCombo,
                 sweep.value(QStringLiteral("band"), QStringLiteral("1")).toInt());
    setComboData(m_notchCombo,
                 sweep.value(QStringLiteral("notch_hz"), QStringLiteral("50")).toInt());
    m_hpSpin->setValue(
        sweep.value(QStringLiteral("highpass_hz"), QStringLiteral("250")).toInt());
    m_spikeLpSpin->setValue(
        sweep.value(QStringLiteral("spike_lowpass_hz"), QStringLiteral("6000")).toInt());
    setComboData(m_orderCombo,
                 sweep.value(QStringLiteral("filter_order"), QStringLiteral("2")).toInt());

    const int thresholdMode =
        sweep.value(QStringLiteral("threshold_mode"), QStringLiteral("0")).toInt();
    setComboData(m_threshModeCombo, thresholdMode);
    if (thresholdMode == 1) {
        m_threshSpin->setRange(-2000.0, 2000.0);
        m_threshSpin->setSuffix(QStringLiteral(" mV"));
        m_threshSpin->setValue(
            sweep.value(QStringLiteral("threshold_value"), QStringLiteral("-50")).toDouble());
    } else {
        m_threshSpin->setRange(1.0, 20.0);
        m_threshSpin->setSuffix(QString());
        m_threshSpin->setValue(
            sweep.value(QStringLiteral("threshold_value"), QStringLiteral("4")).toDouble());
    }
    m_polarityCombo->setCurrentIndex(
        sweep.value(QStringLiteral("negative_polarity"), QStringLiteral("1")).toInt() != 0
            ? 0
            : 1);
    setComboData(m_metricCombo,
                 sweep.value(QStringLiteral("heatmap_metric"), QStringLiteral("0")).toInt());
    setComboData(m_clickModeCombo,
                 sweep.value(QStringLiteral("click_mode"), QStringLiteral("0")).toInt());

    updateFrequencyLimits();
    m_loadingConfig = false;
    applyViewParams();
    applyProcConfig();
    if (m_activityMap) {
        m_activityMap->setMetricMode(m_metricCombo->currentData().toInt());
    }
}

void SweepPlotPanel::scheduleSaveConfig()
{
    if (!m_cfgMgr || m_loadingConfig) return;
    m_saveTimer.start();
}

void SweepPlotPanel::saveConfig() const
{
    if (!m_cfgMgr || m_loadingConfig) return;

    ConfigMap cfg = m_cfgMgr->load();
    ConfigSection &sweep = cfg[QStringLiteral("Sweep")];
    sweep[QStringLiteral("channels")] = m_chEdit->text().trimmed();
    sweep[QStringLiteral("command")] = m_cmdEdit->text().trimmed();
    sweep[QStringLiteral("time_span")] = QString::number(m_spanSpin->value(), 'g', 12);
    sweep[QStringLiteral("sampling_rate")] = QString::number(m_fsSpin->value());
    sweep[QStringLiteral("y_full_scale")] = QString::number(m_yfsSpin->value(), 'g', 12);
    sweep[QStringLiteral("refresh_hz")] = QString::number(m_refreshSpin->value());
    sweep[QStringLiteral("band")] = QString::number(m_bandCombo->currentData().toInt());
    sweep[QStringLiteral("notch_hz")] = QString::number(m_notchCombo->currentData().toInt());
    sweep[QStringLiteral("highpass_hz")] = QString::number(m_hpSpin->value());
    sweep[QStringLiteral("spike_lowpass_hz")] = QString::number(m_spikeLpSpin->value());
    sweep[QStringLiteral("filter_order")] = QString::number(m_orderCombo->currentData().toInt());
    sweep[QStringLiteral("threshold_mode")] =
        QString::number(m_threshModeCombo->currentData().toInt());
    sweep[QStringLiteral("threshold_value")] =
        QString::number(m_threshSpin->value(), 'g', 12);
    sweep[QStringLiteral("negative_polarity")] =
        m_polarityCombo->currentData().toBool() ? QStringLiteral("1")
                                                : QStringLiteral("0");
    sweep[QStringLiteral("heatmap_metric")] =
        QString::number(m_metricCombo->currentData().toInt());
    sweep[QStringLiteral("click_mode")] =
        QString::number(m_clickModeCombo->currentData().toInt());
    m_cfgMgr->save(cfg);
}

void SweepPlotPanel::setStatus(const QString &text, const char *state) {
    if (!m_status) return;
    m_status->setText(text);
    m_status->setProperty("state", state);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void SweepPlotPanel::startStream() {
    if (m_streaming) return;
    const quint64 lifecycleGeneration = ++m_lifecycleGeneration;
    if (!stopThreads()) {
        completeStopWhenReady(QStringLiteral("状态: 已停止"),
                              QStringLiteral("warn"),
                              lifecycleGeneration);
        return;
    }
    if (m_hub) {
        const QSignalBlocker blocker(m_fsSpin);
        m_fsSpin->setValue(
            qBound(m_fsSpin->minimum(),
                   static_cast<int>(std::llround(m_hub->sampleRate())),
                   m_fsSpin->maximum()));
    }
    m_channels = parseChannels(m_chEdit->text());
    if (m_channels.isEmpty() || m_channels.size() > 32) {
        setStatus(QStringLiteral("状态: 请输入1-32个有效且不重复的通道"), "warn");
        return;
    }
    saveConfig();

    // Configure the view (lanes = raw channels, or 2x electrodes in TDM mode).
    m_view->setChannels(displayLanes());
    m_view->setSeriesColors(ThemeManager::seriesPalette(laneCount()));
    applyViewParams();
    applyProcConfig();
    m_view->resetSweep();
    if (m_activityMap) {
        m_activityMap->setSelectedChannels(m_channels);
        m_activityMap->setMetricMode(m_metricCombo->currentData().toInt());
    }

    // Reset stream state and per-channel cursors.
    m_lastTotal.clear();
    m_rxBytes = 0;
    m_parsedFrames = 0;
    m_droppedFrames = 0;
    m_stopFlag = std::make_unique<std::atomic_bool>(false);
    m_rawQueue->clear();

    const QString host = m_networkState->host();
    const int controlPort = m_networkState->port();
    const int dataPort = m_networkState->dataPort();

    // V6: the global session is started from the toolbar; the page only
    // subscribes to whatever it is serving (live or replay).
    qint64 initialFrameIndex = 0;
    if (m_hub) {
        initialFrameIndex = m_hub->addSubscriberWithFrameOrigin(m_rawQueue);
    }
    {
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->channels = m_channels;
        m_streamState->buffers.clear();
        m_streamState->totalSamples.clear();
        for (int ch : m_channels) {
            m_streamState->totalSamples[ch] = initialFrameIndex;
        }
        m_streamState->droppedFrames = 0;
        m_streamState->maxSamples = 16384;
        m_streamState->metricWindowFrames = qMax(
            64, static_cast<int>(std::llround(m_fsSpin->value() * 0.1)));
        m_streamState->timelineEpoch = m_hub ? m_hub->timelineEpoch() : 0;
    }

    m_sorter = new DataSorter(m_rawQueue, m_streamState, m_stopFlag.get(),
                              m_hub ? m_hub->timelineEpochCounter() : nullptr,
                              m_hub ? m_hub->referenceModeCounter() : nullptr,
                              m_hub ? m_hub->timelineFrameOriginCounter() : nullptr,
                              initialFrameIndex,
                              this);
    m_firstRefreshPending = true;
    connect(m_sorter, &DataSorter::framesParsed, this,
            [this, lifecycleGeneration](int frameCount, int) {
        if (lifecycleGeneration != m_lifecycleGeneration) return;
        m_parsedFrames += frameCount;
        if (m_firstRefreshPending && m_streaming) {
            m_firstRefreshPending = false;
            QTimer::singleShot(0, this, &SweepPlotPanel::refresh);
        }
    });
    m_sorter->start();

    m_streaming = true;
    m_paused = false;
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_btnPause->setEnabled(true);
    m_btnPause->setText(QStringLiteral("暂停"));
    m_refreshTimer.start(qMax(16, 1000 / qMax(1, m_refreshSpin->value())));
    setStatus(QStringLiteral("状态: 扫描采集中 %1 (控制:%2 数据:%3)")
                  .arg(host).arg(controlPort).arg(dataPort),
              "ok");
}

void SweepPlotPanel::togglePause() {
    if (!m_streaming) return;
    m_paused = !m_paused;
    m_btnPause->setText(m_paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
    setStatus(m_paused ? QStringLiteral("状态: 已暂停") : QStringLiteral("状态: 扫描采集中"),
              m_paused ? "warn" : "ok");
}

void SweepPlotPanel::stopStream() {
    if (!m_streaming && !m_receiver && !m_sorter) return;
    const quint64 lifecycleGeneration = ++m_lifecycleGeneration;
    if (m_refreshTimer.isActive()) m_refreshTimer.stop();
    m_streaming = false;
    m_paused = false;
    m_firstRefreshPending = false;
    if (m_stopFlag) m_stopFlag->store(true);
    if (m_rawQueue) m_rawQueue->wakeAll();
    completeStopWhenReady(QStringLiteral("状态: 已停止"),
                          QStringLiteral("warn"),
                          lifecycleGeneration);
}

bool SweepPlotPanel::stopThreads() {
    if (m_hub) m_hub->removeSubscriber(m_rawQueue);
    if (m_stopFlag) m_stopFlag->store(true);
    if (m_rawQueue) m_rawQueue->wakeAll();
    bool allStopped = true;
    if (m_receiver) {
        if (!m_receiver->wait(250)) {
            m_receiver->requestInterruption();
            if (m_rawQueue) m_rawQueue->wakeAll();
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
            if (m_rawQueue) m_rawQueue->wakeAll();
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

void SweepPlotPanel::completeStopWhenReady(const QString &finalText,
                                           const QString &state,
                                           quint64 lifecycleGeneration)
{
    if (lifecycleGeneration != m_lifecycleGeneration) return;
    const bool stopped = stopThreads();
    m_btnStart->setEnabled(stopped);
    m_btnStop->setEnabled(false);
    m_btnPause->setEnabled(false);
    if (stopped) {
        setStatus(finalText, state.toLatin1().constData());
        return;
    }

    setStatus(QStringLiteral("状态: 正在停止…"), "warn");
    QTimer::singleShot(250, this, [this, finalText, state, lifecycleGeneration]() {
        completeStopWhenReady(finalText, state, lifecycleGeneration);
    });
}

void SweepPlotPanel::refresh() {
    if (!m_streaming || m_paused || !m_view) return;
    if (m_hub) {
        const SessionHub::Statistics stats = m_hub->statistics();
        m_rxBytes = stats.receivedBytes;
        m_droppedFrames =
            stats.ingressDroppedFrames + stats.subscriberDroppedFrames;
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->droppedFrames = static_cast<quint64>(qMax<qint64>(0, m_droppedFrames));
    }

    const int nCh = m_channels.size();
    QVector<QVector<double>> batch(nCh);
    QVector<qint64> absStart(nCh, 0);   // absolute frame index of batch[i][0]
    QVector<RealtimeChannelMetric> metrics;
    QVector<RealtimeChannelMetric> tdmMetrics;
    qint64 maxNew = 0;
    {
        QMutexLocker locker(&m_streamState->lock);
        metrics = m_streamState->metrics;
        tdmMetrics = m_streamState->tdmMetrics;
        for (int i = 0; i < nCh; ++i) {
            const int ch = m_channels[i];
            auto it = m_streamState->buffers.constFind(ch);
            if (it == m_streamState->buffers.cend()) continue;
            const QQueue<double> &q = it.value();
            const qint64 total = m_streamState->totalSamples.value(ch, 0);
            const qint64 last = m_lastTotal.value(ch, 0);
            qint64 newCount = total - last;
            if (newCount < 0) newCount = 0;
            if (newCount > q.size()) newCount = q.size();
            // The read window's oldest sample is the true absolute frame index,
            // even after a clamp/clear — this is what fixes the TDM parity.
            const qint64 startIndex = total - newCount;
            // In TDM mode consume a multiple of four samples so the two visible ELE
            // lanes come out equal length (appendBatch advances all lanes by
            // the shortest); the odd leftover waits one refresh (~ms).
            qint64 consume = m_tdmEnabled
                                 ? newCount - (newCount % kTdmPhaseCount)
                                 : newCount;
            QVector<double> data;
            data.reserve(static_cast<int>(consume));
            const int base = q.size() - static_cast<int>(newCount);
            for (int j = 0; j < consume; ++j) {
                data.push_back(q.at(base + j));
            }
            batch[i] = data;
            absStart[i] = startIndex;
            m_lastTotal[ch] = startIndex + consume;
            maxNew = qMax(maxNew, consume);
        }
    }

    if (maxNew > 0) {
        const int lanes = laneCount();
        QVector<QVector<double>> laneIn(lanes);
        if (m_tdmEnabled) {
            // Split all four hardware phases, retaining the selected 0/2 or
            // 1/3 pair. The unselected pair is intentionally not displayed.
            for (int i = 0; i < nCh; ++i) {
                const QVector<double> &data = batch[i];
                QVector<double> first, second;
                const QPair<int, int> pair = tdmLocalElePair(m_tdmEvenFirst);
                first.reserve(data.size() / kTdmPhaseCount + 1);
                second.reserve(data.size() / kTdmPhaseCount + 1);
                for (int k = 0; k < data.size(); ++k) {
                    const int phase = tdmPhaseForFrame(absStart[i] + k);
                    if (phase == pair.first) {
                        first.push_back(data[k]);
                    } else if (phase == pair.second) {
                        second.push_back(data[k]);
                    }
                }
                laneIn[2 * i] = first;
                laneIn[2 * i + 1] = second;
            }
        } else {
            laneIn = batch;
        }

        QVector<QVector<double>> dispBatch(lanes);
        QVector<QVector<bool>> flagBatch(lanes);
        for (int L = 0; L < lanes; ++L) {
            QVector<double> disp;
            QVector<bool> flags;
            m_proc.processChannel(L, laneIn[L], disp, flags);
            dispBatch[L] = disp;
            flagBatch[L] = flags;
        }
        m_view->appendBatch(dispBatch, flagBatch);
    }

    if (m_activityMap) {
        // TDM: per-electrode metrics; otherwise raw per-channel metrics.
        if (m_tdmEnabled) {
            if (!tdmMetrics.isEmpty()) m_activityMap->setTdmMetrics(tdmMetrics);
        } else if (!metrics.isEmpty()) {
            m_activityMap->setMetrics(metrics);
        }
    }

    setStatus(QStringLiteral("状态: 扫描采集中 | RX %1 B | frames %2 | dropped %3")
                  .arg(m_rxBytes).arg(m_parsedFrames).arg(m_droppedFrames),
              m_droppedFrames > 0 ? "warn" : "ok");
}

void SweepPlotPanel::setWaveTheme(const QMap<QString, QString> &palette) {
    if (m_view) m_view->setThemePalette(palette);
    if (m_activityMap) m_activityMap->setPaletteColors(palette);
}

void SweepPlotPanel::applyTdmState(bool enabled, bool evenFirst) {
    const bool changed = (enabled != m_tdmEnabled) || (evenFirst != m_tdmEvenFirst);
    m_tdmEnabled = enabled;
    m_tdmEvenFirst = evenFirst;
    if (m_tdmCheck) {
        const QSignalBlocker b(m_tdmCheck);
        m_tdmCheck->setChecked(enabled);
    }
    if (m_tdmPhaseCombo) {
        const QSignalBlocker b(m_tdmPhaseCombo);
        setComboData(m_tdmPhaseCombo, evenFirst ? 0 : 1);
        m_tdmPhaseCombo->setEnabled(enabled);
    }
    if (m_activityMap) {
        m_activityMap->setTdmDisplay(enabled, evenFirst);
    }
    if (!changed) {
        return;
    }
    // Rebuild the display pipeline for the new lane layout / decimated rate.
    // Skip when no channels are selected yet — startStream builds it fresh.
    if (m_view && !m_channels.isEmpty()) {
        m_view->setChannels(displayLanes());
        m_view->setSeriesColors(ThemeManager::seriesPalette(laneCount()));
        applyViewParams();
        applyProcConfig();
        m_view->resetSweep();
    }
    m_proc.reset();
    // Drop buffered samples so pre-toggle data can't mix into the new mode, but
    // keep totalSamples so absolute frame numbering (TDM phase) stays intact.
    m_lastTotal.clear();
    if (m_streamState) {
        QMutexLocker locker(&m_streamState->lock);
        m_streamState->buffers.clear();
    }
}

void SweepPlotPanel::setSessionHub(SessionHub *hub) {
    m_hub = hub;
    if (!m_hub) return;
    if (TdmContext *tdm = m_hub->tdmContext()) {
        applyTdmState(tdm->enabled(), tdm->pair02());
        connect(tdm, &TdmContext::changed, this, &SweepPlotPanel::applyTdmState);
    }
    connect(m_hub, &SessionHub::connectionStateChanged, this, [this](bool connected) {
        if (!connected && m_streaming) {
            setStatus(QStringLiteral("状态: 采集连接已断开"), "warn");
        }
    });
    connect(m_hub, &SessionHub::connectionEvent, this, [this](const QString &msg) {
        if (m_streaming) setStatus(QStringLiteral("状态: %1").arg(msg), "ok");
    });
    connect(m_hub,
            &SessionHub::timelineReset,
            this,
            [this](quint64 epoch, qint64 targetFrame) {
        resetTimelineState(epoch, targetFrame);
    });
}

void SweepPlotPanel::resetTimelineState(quint64 epoch, qint64 targetFrame)
{
    // Seek barrier: sorter keeps running and drops stale chunks itself; just
    // clear the buffers, sweep canvas and spike processor state.
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
    m_lastTotal.clear();
    m_proc.reset();
    if (m_view) m_view->resetSweep();
    if (m_activityMap) m_activityMap->clearMetrics();
    m_parsedFrames = 0;
    m_droppedFrames = 0;
    m_firstRefreshPending = m_streaming;
}

void SweepPlotPanel::onActivated() {
    if (m_hub && m_hub->isRunning() && !m_streaming) {
        startStream();
    }
}

void SweepPlotPanel::onDeactivated() {
    if (m_streaming || m_sorter) {
        stopStream();
    }
}

void SweepPlotPanel::stopAcquisition() {
    stopStream();
}

void SweepPlotPanel::shutdown() {
    ++m_lifecycleGeneration;
    m_firstRefreshPending = false;
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveConfig();
    }
    if (m_refreshTimer.isActive()) m_refreshTimer.stop();
    m_streaming = false;
    if (m_stopFlag) m_stopFlag->store(true);
    if (m_rawQueue) m_rawQueue->wakeAll();
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

}  // namespace ccv2
