#include "ui/spike_panel.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QStringList>
#include <QVBoxLayout>

#include "config/config_manager.h"
#include "core/channel_routing.h"
#include "core/constants.h"
#include "core/tdm_context.h"
#include "service/session_hub.h"
#include "ui/widgets/spike_detail_window.h"
#include "ui/widgets/spike_grid_view.h"

namespace ccv2 {

namespace {

constexpr int kQueueChunks = 256;

void setStatusLine(QLabel *label, const QString &text, const char *state) {
    if (!label) return;
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QLabel *fieldLabel(const QString &text) {
    auto *l = new QLabel(text);
    l->setProperty("role", "field-label");
    return l;
}

QMap<int, double> parseThresholdOverrides(const QString &text) {
    QMap<int, double> result;
    const QStringList entries = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const QStringList parts = entry.split(QLatin1Char(':'));
        if (parts.size() != 2) continue;
        bool laneOk = false;
        bool valueOk = false;
        const int lane = parts[0].toInt(&laneOk);
        const double value = parts[1].toDouble(&valueOk);
        if (laneOk && valueOk && lane >= 0 && value > 0.0 &&
            std::isfinite(value)) {
            result.insert(lane, value);
        }
    }
    return result;
}

QString serializeThresholdOverrides(const QMap<int, double> &overrides) {
    QStringList entries;
    entries.reserve(overrides.size());
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        entries.push_back(
            QStringLiteral("%1:%2").arg(it.key()).arg(it.value(), 0, 'f', 3));
    }
    return entries.join(QLatin1Char(';'));
}

}  // namespace

SpikePanel::SpikePanel(ConfigManager *cfgMgr, QWidget *parent)
    : QWidget(parent),
      m_cfgMgr(cfgMgr),
      m_rawQueue(std::make_shared<ThreadSafeQueue<QByteArray>>(kQueueChunks)) {
    buildUi();
    loadConfig();

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { saveConfig(); });

    connect(&m_refreshTimer, &QTimer::timeout, this, &SpikePanel::refresh);
    m_refreshTimer.setInterval(50);  // 20 Hz snippet pull
}

SpikePanel::~SpikePanel() {
    shutdown();
}

int SpikePanel::laneCount() const {
    return kChannelsTotal * (m_tdmEnabled ? 2 : 1);
}

double SpikePanel::procSampleRate() const {
    const double fs = m_hub ? m_hub->sampleRate() : 20000.0;
    return m_tdmEnabled ? fs / kTdmPhaseCount : fs;
}

void SpikePanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // --- control bar -------------------------------------------------------
    auto *controls = new QWidget;
    controls->setProperty("role", "page-control-panel");
    auto *cbar = new QHBoxLayout(controls);
    cbar->setContentsMargins(0, 0, 0, 0);
    cbar->setSpacing(8);

    cbar->addWidget(fieldLabel(QStringLiteral("换算增益")));
    m_gainCombo = new QComboBox;
    m_gainCombo->addItem(QStringLiteral("60×"), 60.0);
    m_gainCombo->addItem(QStringLiteral("180×"), 180.0);
    m_gainCombo->setToolTip(
        QStringLiteral("ADC 前端输出电压除以该增益，得到输入等效 Spike 电压；"
                       "应与硬件 REC 增益设置一致"));
    cbar->addWidget(m_gainCombo);

    cbar->addWidget(fieldLabel(QStringLiteral("阈值")));
    m_threshModeCombo = new QComboBox;
    m_threshModeCombo->addItem(QStringLiteral("RMS 倍数"), 0);
    m_threshModeCombo->addItem(QStringLiteral("绝对值 µV"), 1);
    cbar->addWidget(m_threshModeCombo);
    m_threshSpin = new QDoubleSpinBox;
    m_threshSpin->setRange(0.5, 500.0);
    m_threshSpin->setDecimals(2);
    m_threshSpin->setValue(4.0);
    m_threshSpin->setSuffix(QStringLiteral(" ×RMS"));
    m_threshSpin->setToolTip(QStringLiteral("RMS 倍数模式: 阈值 = 倍数 × 该通道噪声 RMS"));
    cbar->addWidget(m_threshSpin);

    m_polarityCombo = new QComboBox;
    m_polarityCombo->addItem(QStringLiteral("负向"), 1);
    m_polarityCombo->addItem(QStringLiteral("正向"), 0);
    cbar->addWidget(m_polarityCombo);

    cbar->addWidget(fieldLabel(QStringLiteral("高通")));
    m_hpSpin = new QSpinBox;
    m_hpSpin->setRange(50, 1000);
    m_hpSpin->setValue(250);
    m_hpSpin->setSuffix(QStringLiteral(" Hz"));
    cbar->addWidget(m_hpSpin);

    m_notchCombo = new QComboBox;
    m_notchCombo->addItem(QStringLiteral("陷波 关"), 0);
    m_notchCombo->addItem(QStringLiteral("陷波 50Hz"), 50);
    m_notchCombo->addItem(QStringLiteral("陷波 60Hz"), 60);
    cbar->addWidget(m_notchCombo);

    cbar->addWidget(fieldLabel(QStringLiteral("窗口")));
    m_preSpin = new QDoubleSpinBox;
    m_preSpin->setRange(0.1, 2.0);
    m_preSpin->setDecimals(2);
    m_preSpin->setSingleStep(0.1);
    m_preSpin->setValue(0.4);
    m_preSpin->setSuffix(QStringLiteral(" ms前"));
    cbar->addWidget(m_preSpin);
    m_postSpin = new QDoubleSpinBox;
    m_postSpin->setRange(0.2, 4.0);
    m_postSpin->setDecimals(2);
    m_postSpin->setSingleStep(0.1);
    m_postSpin->setValue(1.2);
    m_postSpin->setSuffix(QStringLiteral(" ms后"));
    cbar->addWidget(m_postSpin);

    cbar->addWidget(fieldLabel(QStringLiteral("留存")));
    m_retainSpin = new QSpinBox;
    m_retainSpin->setRange(20, 2000);
    m_retainSpin->setSingleStep(50);
    m_retainSpin->setValue(200);
    m_retainSpin->setSuffix(QStringLiteral(" 条/通道"));
    m_retainSpin->setToolTip(QStringLiteral("每个通道叠加保留的波形条数"));
    cbar->addWidget(m_retainSpin);
    cbar->addStretch(1);
    root->addWidget(controls);

    // --- display options ---------------------------------------------------
    auto *viewOpts = new QWidget;
    viewOpts->setProperty("role", "page-control-panel");
    auto *vbar = new QHBoxLayout(viewOpts);
    vbar->setContentsMargins(0, 0, 0, 0);
    vbar->setSpacing(8);

    vbar->addWidget(fieldLabel(QStringLiteral("量程")));
    m_yScaleSpin = new QDoubleSpinBox;
    m_yScaleSpin->setRange(10.0, 5000.0);
    m_yScaleSpin->setDecimals(0);
    m_yScaleSpin->setSingleStep(25.0);
    m_yScaleSpin->setValue(200.0);
    m_yScaleSpin->setSuffix(QStringLiteral(" µV"));
    vbar->addWidget(m_yScaleSpin);
    m_autoScaleCheck = new QCheckBox(QStringLiteral("自动量程"));
    m_autoScaleCheck->setToolTip(QStringLiteral("每格按已留存波形的峰值自适应"));
    vbar->addWidget(m_autoScaleCheck);
    m_fadeCheck = new QCheckBox(QStringLiteral("旧波形淡出"));
    m_fadeCheck->setChecked(true);
    vbar->addWidget(m_fadeCheck);

    vbar->addWidget(fieldLabel(QStringLiteral("列数")));
    m_columnsSpin = new QSpinBox;
    m_columnsSpin->setRange(0, 64);
    m_columnsSpin->setValue(0);
    m_columnsSpin->setSpecialValueText(QStringLiteral("自动"));
    vbar->addWidget(m_columnsSpin);

    m_btnPause = new QPushButton(QStringLiteral("暂停显示"));
    m_btnPause->setProperty("variant", "secondary");
    m_btnPause->setCheckable(true);
    vbar->addWidget(m_btnPause);
    m_btnClear = new QPushButton(QStringLiteral("清空留存"));
    m_btnClear->setProperty("variant", "secondary");
    vbar->addWidget(m_btnClear);
    vbar->addStretch(1);
    m_status = new QLabel(QStringLiteral("状态: 未开始"));
    m_status->setProperty("role", "status-line");
    m_status->setProperty("state", "warn");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setMinimumWidth(0);
    vbar->addWidget(m_status);
    root->addWidget(viewOpts);

    // --- full-array grid ---------------------------------------------------
    auto *gridWrap = new QWidget;
    gridWrap->setProperty("role", "plot-area");
    auto *gridLayout = new QVBoxLayout(gridWrap);
    gridLayout->setContentsMargins(6, 6, 6, 6);
    gridLayout->setSpacing(4);
    auto *gridTitle = new QLabel(QStringLiteral("全阵列 Spike 留存（输入等效）"));
    gridTitle->setObjectName(QStringLiteral("title"));
    auto *gridHint = new QLabel(
        QStringLiteral("每格 = 一个电极；点击后在独立窗口查看并调整该通道阈值"));
    gridHint->setProperty("role", "caption");
    gridHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    gridHint->setMinimumWidth(0);
    auto *gridHeader = new QHBoxLayout;
    gridHeader->addWidget(gridTitle);
    gridHeader->addStretch(1);
    gridHeader->addWidget(gridHint);
    gridLayout->addLayout(gridHeader);
    m_grid = new SpikeGridView;
    // The array grid is an activity overview. One fresh waveform per lane and
    // refresh is enough; the pop-out detail view keeps the denser history.
    m_grid->setSnippetDrawLimits(1, 8);
    gridLayout->addWidget(m_grid, 1);
    root->addWidget(gridWrap, 1);

    m_store.configure(kChannelsTotal, 33, 200);
    m_grid->setStore(&m_store);
    m_detailWindow = new SpikeDetailWindow(this);
    m_detailWindow->setStore(&m_store);
    updateLaneLabels();

    connect(m_grid, &SpikeGridView::laneClicked,
            this, &SpikePanel::showLaneDetail);
    connect(m_detailWindow, &SpikeDetailWindow::thresholdOverrideChanged,
            this, &SpikePanel::setLaneThresholdOverride);
    connect(m_detailWindow, &SpikeDetailWindow::displayOptionsChanged, this,
            [this](double yFullScaleUv, bool autoScale, bool fade,
                   int overlayCount) {
        m_detailYScaleUv = yFullScaleUv;
        m_detailAutoScale = autoScale;
        m_detailFade = fade;
        m_detailOverlayCount = overlayCount;
        scheduleSaveConfig();
    });

    // Detector/window changes must rebuild the worker; view-only options do not.
    auto restart = [this]() { scheduleSaveConfig(); restartDetection(); };
    connect(m_gainCombo, &QComboBox::currentIndexChanged, this,
            [restart](int) { restart(); });
    connect(m_threshModeCombo, &QComboBox::currentIndexChanged, this, [this, restart](int) {
        const bool absolute = m_threshModeCombo->currentData().toInt() == 1;
        m_threshSpin->setSuffix(absolute ? QStringLiteral(" µV") : QStringLiteral(" ×RMS"));
        m_threshSpin->setValue(absolute ? 50.0 : 4.0);
        restart();
    });
    connect(m_threshSpin, &QDoubleSpinBox::valueChanged, this, [restart](double) { restart(); });
    connect(m_polarityCombo, &QComboBox::currentIndexChanged, this, [restart](int) { restart(); });
    connect(m_hpSpin, &QSpinBox::valueChanged, this, [restart](int) { restart(); });
    connect(m_notchCombo, &QComboBox::currentIndexChanged, this, [restart](int) { restart(); });
    connect(m_preSpin, &QDoubleSpinBox::valueChanged, this, [restart](double) { restart(); });
    connect(m_postSpin, &QDoubleSpinBox::valueChanged, this, [restart](double) { restart(); });
    connect(m_retainSpin, &QSpinBox::valueChanged, this, [restart](int) { restart(); });

    connect(m_yScaleSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_grid->setYFullScaleMicrovolts(v);
        scheduleSaveConfig();
    });
    connect(m_autoScaleCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_grid->setAutoScale(on);
        scheduleSaveConfig();
    });
    connect(m_fadeCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_grid->setFadeEnabled(on);
        scheduleSaveConfig();
    });
    connect(m_columnsSpin, &QSpinBox::valueChanged, this, [this](int v) {
        m_grid->setColumns(v);
        scheduleSaveConfig();
    });
    connect(m_btnPause, &QPushButton::toggled, this, [this](bool on) {
        m_paused = on;
        m_btnPause->setText(on ? QStringLiteral("继续显示") : QStringLiteral("暂停显示"));
    });
    connect(m_btnClear, &QPushButton::clicked, this, &SpikePanel::clearAll);
}

SpikeDetectConfig SpikePanel::currentConfig() const {
    SpikeDetectConfig cfg;
    const double fs = procSampleRate();
    cfg.inputGain =
        m_gainCombo ? m_gainCombo->currentData().toDouble() : 60.0;
    cfg.proc.sampleRate = fs;
    cfg.proc.band = SpikeProcConfig::SpikeBand;
    cfg.proc.filterOrder = 2;
    cfg.proc.notchHz = m_notchCombo ? m_notchCombo->currentData().toInt() : 0;
    cfg.proc.highpassHz = m_hpSpin ? m_hpSpin->value() : 250.0;
    cfg.proc.spikeLowpassHz = qMin(6000.0, fs * 0.45);
    cfg.proc.negativePolarity =
        m_polarityCombo ? m_polarityCombo->currentData().toInt() == 1 : true;
    cfg.proc.absoluteThreshold =
        m_threshModeCombo ? m_threshModeCombo->currentData().toInt() == 1 : false;
    if (cfg.proc.absoluteThreshold) {
        const double magnitude = (m_threshSpin ? m_threshSpin->value() : 50.0) * 1e-6;
        cfg.proc.absThresholdV = cfg.proc.negativePolarity ? -magnitude : magnitude;
    } else {
        cfg.proc.rmsMultiple = m_threshSpin ? m_threshSpin->value() : 4.0;
    }
    cfg.proc.refractoryMs = 1.0;
    cfg.preSamples = qMax(1, static_cast<int>(std::lround((m_preSpin ? m_preSpin->value() : 0.4) * fs / 1000.0)));
    cfg.postSamples = qMax(1, static_cast<int>(std::lround((m_postSpin ? m_postSpin->value() : 1.2) * fs / 1000.0)));
    cfg.tdmEnabled = m_tdmEnabled;
    cfg.tdmPair02 = m_tdmPair02;
    return cfg;
}

void SpikePanel::updateLaneLabels() {
    const int lanes = laneCount();
    QVector<int> labels(lanes);
    for (int i = 0; i < lanes; ++i) {
        if (m_tdmEnabled) {
            const int ch = i / 2;
            const QPair<int, int> pair = tdmLocalElePair(m_tdmPair02);
            const int localEle = (i % 2 == 0) ? pair.first : pair.second;
            labels[i] = globalEleForChannelLocalEle(ch, localEle);
        } else {
            labels[i] = i;
        }
    }
    if (m_grid) m_grid->setLaneLabels(labels);
}

QMap<int, double> &SpikePanel::activeThresholdOverrides() {
    return m_tdmEnabled ? m_tdmThresholdOverridesUv
                        : m_adcThresholdOverridesUv;
}

const QMap<int, double> &SpikePanel::activeThresholdOverrides() const {
    return m_tdmEnabled ? m_tdmThresholdOverridesUv
                        : m_adcThresholdOverridesUv;
}

int SpikePanel::thresholdKeyForLane(int lane) const {
    if (!m_tdmEnabled) {
        return lane;
    }
    const QPair<int, int> pair = tdmLocalElePair(m_tdmPair02);
    const int localEle = (lane % 2 == 0) ? pair.first : pair.second;
    return globalEleForChannelLocalEle(lane / 2, localEle);
}

void SpikePanel::showLaneDetail(int lane) {
    if (!m_detailWindow || lane < 0 || lane >= laneCount()) return;
    m_selectedLane = lane;
    m_grid->setSelectedLane(lane);

    const QMap<int, double> &overrides = activeThresholdOverrides();
    const int thresholdKey = thresholdKeyForLane(lane);
    const bool overrideEnabled = overrides.contains(thresholdKey);
    double magnitudeUv = overrides.value(thresholdKey, 0.0);
    if (!overrideEnabled) {
        magnitudeUv = std::abs(m_store.threshold(lane)) * 1e6;
        if (magnitudeUv <= 0.0 &&
            m_threshModeCombo->currentData().toInt() == 1) {
            magnitudeUv = m_threshSpin->value();
        }
    }
    if (magnitudeUv <= 0.0) magnitudeUv = 50.0;

    const QString name =
        m_tdmEnabled
            ? QStringLiteral("电极 %1 · ADC %2 · 相位 %3")
                  .arg(globalEleForChannelLocalEle(
                      lane / 2,
                      lane % 2 == 0 ? tdmLocalElePair(m_tdmPair02).first
                                    : tdmLocalElePair(m_tdmPair02).second))
                  .arg(lane / 2)
                  .arg(lane % 2 == 0 ? tdmLocalElePair(m_tdmPair02).first
                                     : tdmLocalElePair(m_tdmPair02).second)
            : QStringLiteral("通道 %1").arg(lane);
    const bool negative =
        m_polarityCombo && m_polarityCombo->currentData().toInt() == 1;
    m_detailWindow->setLane(lane, name, overrideEnabled, magnitudeUv, negative);
    m_detailWindow->setDisplayOptions(
        m_detailYScaleUv, m_detailAutoScale, m_detailFade,
        m_detailOverlayCount);
    m_detailWindow->show();
    m_detailWindow->raise();
    m_detailWindow->activateWindow();
}

void SpikePanel::setLaneThresholdOverride(int lane, bool enabled,
                                          double magnitudeUv) {
    if (lane < 0 || lane >= laneCount() || !std::isfinite(magnitudeUv)) return;
    magnitudeUv = qBound(1.0, magnitudeUv, 5000.0);
    QMap<int, double> &overrides = activeThresholdOverrides();
    const int thresholdKey = thresholdKeyForLane(lane);
    if (enabled) {
        overrides.insert(thresholdKey, magnitudeUv);
    } else {
        overrides.remove(thresholdKey);
    }

    if (m_worker) {
        const bool negative =
            m_polarityCombo && m_polarityCombo->currentData().toInt() == 1;
        const double thresholdV =
            (negative ? -magnitudeUv : magnitudeUv) * 1e-6;
        m_worker->setLaneThresholdOverride(lane, enabled, thresholdV);
    }
    scheduleSaveConfig();
}

void SpikePanel::startDetection() {
    if (m_running || !m_hub) return;
    if (!m_hub->isRunning()) {
        setStatus(QStringLiteral("状态: 会话未运行 — 请先在顶部开始接收或加载 BIN"), "warn");
        return;
    }
    if (!stopWorker()) {
        setStatus(QStringLiteral("状态: 上一轮检测线程尚未退出，请稍候重试"), "error");
        return;
    }

    const SpikeDetectConfig cfg = currentConfig();
    const int lanes = laneCount();
    const int len = cfg.preSamples + cfg.postSamples + 1;
    const int requestedRetention = m_retainSpin ? m_retainSpin->value() : 200;
    if (!m_store.configure(lanes, len, requestedRetention)) {
        setStatus(QStringLiteral("状态: Spike 留存内存不足，请缩短窗口或降低采样率"), "error");
        return;
    }
    m_store.resetTimeline(m_hub->timelineEpoch());
    if (m_retainSpin && m_store.capacity() < requestedRetention) {
        const QSignalBlocker blocker(m_retainSpin);
        m_retainSpin->setMinimum(1);
        m_retainSpin->setValue(m_store.capacity());
        m_retainSpin->setToolTip(QStringLiteral("已按 128 MiB 留存内存上限调整条数"));
        scheduleSaveConfig();
    }
    m_lastCounts.fill(0, lanes);
    m_rates.fill(0.0, lanes);
    updateLaneLabels();
    m_grid->setTimebase(procSampleRate(), cfg.preSamples);
    m_grid->setStore(&m_store);
    if (m_detailWindow) {
        m_detailWindow->setStore(&m_store);
        m_detailWindow->setTimebase(procSampleRate(), cfg.preSamples);
    }
    m_grid->setColumns(m_columnsSpin ? m_columnsSpin->value() : 0);
    m_grid->setYFullScaleMicrovolts(m_yScaleSpin ? m_yScaleSpin->value() : 200.0);
    m_grid->setAutoScale(m_autoScaleCheck && m_autoScaleCheck->isChecked());
    m_grid->setFadeEnabled(m_fadeCheck && m_fadeCheck->isChecked());
    if (m_detailWindow) {
        m_detailWindow->setDisplayOptions(
            m_detailYScaleUv, m_detailAutoScale, m_detailFade,
            m_detailOverlayCount);
    }

    m_framesProcessed = 0;
    m_stopFlag = std::make_unique<std::atomic_bool>(false);
    m_rawQueue->clear();
    const qint64 initialFrame = m_hub->addSubscriberWithFrameOrigin(m_rawQueue);

    m_worker = new SpikeDetectWorker(m_rawQueue,
                                     &m_store,
                                     m_stopFlag.get(),
                                     cfg,
                                     m_hub->timelineEpochCounter(),
                                     m_hub->referenceModeCounter(),
                                     initialFrame,
                                      this, m_hub->timelineFrameOriginCounter());
    SpikeDetectWorker *const active = m_worker;
    connect(m_worker, &SpikeDetectWorker::framesProcessed, this,
            [this, active](qint64 frames) {
        if (m_worker != active) return;  // stale worker's queued signal
        m_framesProcessed += frames;
    });
    const bool negative = cfg.proc.negativePolarity;
    const QMap<int, double> &overrides = activeThresholdOverrides();
    for (int lane = 0; lane < lanes; ++lane) {
        const auto it = overrides.constFind(thresholdKeyForLane(lane));
        if (it == overrides.cend()) continue;
        const double thresholdV =
            (negative ? -it.value() : it.value()) * 1e-6;
        m_worker->setLaneThresholdOverride(lane, true, thresholdV);
    }
    m_worker->start();

    m_rateTimer.start();
    m_running = true;
    m_refreshTimer.start();
    setStatus(QStringLiteral("状态: 检测中 (%1 电极，输入等效 %2×)")
                  .arg(lanes)
                  .arg(cfg.inputGain, 0, 'f', 0),
              "ok");
}

bool SpikePanel::stopWorker() {
    if (m_hub) {
        m_hub->removeSubscriber(m_rawQueue);
    }
    if (m_stopFlag) m_stopFlag->store(true);
    if (m_rawQueue) m_rawQueue->wakeAll();
    if (!m_worker) return true;
    // The worker's queue pop times out after 200ms; wait past that before
    // giving up so we never rebuild its stop flag underneath a live thread.
    if (!m_worker->wait(800)) {
        m_rawQueue->wakeAll();
        if (!m_worker->wait(400)) {
            return false;
        }
    }
    m_worker->deleteLater();
    m_worker = nullptr;
    return true;
}

void SpikePanel::restartDetection() {
    if (!m_running) return;
    m_refreshTimer.stop();
    m_running = false;
    if (!stopWorker()) {
        setStatus(QStringLiteral("状态: 检测线程停止超时，请稍候重试"), "error");
        return;
    }
    startDetection();
}

void SpikePanel::clearAll() {
    m_store.clear();
    const int lanes = laneCount();
    m_lastCounts.fill(0, lanes);
    m_rates.fill(0.0, lanes);
    if (m_grid) m_grid->clearTraces();
    if (m_detailWindow) m_detailWindow->clearTraces();
}

void SpikePanel::refresh() {
    if (!m_running) return;
    if (!m_paused) {
        m_grid->pullNewSnippets();
        if (m_detailWindow) m_detailWindow->pullNewSnippets();
    }

    QVector<qint64> totals;
    QVector<double> thresholds;
    QVector<double> rms;
    m_store.snapshotStats(&totals, &thresholds, &rms);

    const double elapsed = m_rateTimer.isValid() ? m_rateTimer.elapsed() / 1000.0 : 0.0;
    if (elapsed >= 0.5) {
        m_rateTimer.restart();
        if (m_lastCounts.size() != totals.size()) m_lastCounts.fill(0, totals.size());
        if (m_rates.size() != totals.size()) m_rates.fill(0.0, totals.size());
        qint64 grandTotal = 0;
        int activeLanes = 0;
        for (int i = 0; i < totals.size(); ++i) {
            const qint64 delta = qMax<qint64>(0, totals[i] - m_lastCounts[i]);
            m_lastCounts[i] = totals[i];
            // Light smoothing so the per-cell readout does not flicker.
            m_rates[i] = 0.5 * m_rates[i] + 0.5 * (delta / elapsed);
            grandTotal += totals[i];
            if (m_rates[i] >= 1.0) ++activeLanes;
        }
        m_grid->setRates(m_rates);
        setStatus(QStringLiteral("状态: 检测中 | 帧 %1 | 累计 spike %2 | 活跃电极 %3/%4")
                      .arg(m_framesProcessed)
                      .arg(grandTotal)
                      .arg(activeLanes)
                      .arg(totals.size()),
                  "ok");
    }
    m_grid->setThresholds(thresholds);
    if (m_detailWindow && m_detailWindow->isVisible()) {
        m_detailWindow->updateData(totals, thresholds, rms, m_rates);
    }
}

void SpikePanel::setSessionHub(SessionHub *hub) {
    m_hub = hub;
    if (!m_hub) return;
    if (TdmContext *tdm = m_hub->tdmContext()) {
        applyTdmState(tdm->enabled(), tdm->pair02());
        connect(tdm, &TdmContext::changed, this, &SpikePanel::applyTdmState);
    }
    connect(m_hub, &SessionHub::connectionStateChanged, this, [this](bool connected) {
        if (!connected && m_running) {
            setStatus(QStringLiteral("状态: 采集连接已断开"), "warn");
        }
    });
    connect(m_hub, &SessionHub::timelineReset, this, [this](quint64 epoch, qint64) {
        // A seek/reset invalidates the retained waveforms' alignment.
        m_store.resetTimeline(epoch);
        clearAll();
    });
}

void SpikePanel::applyTdmState(bool enabled, bool pair02) {
    if (m_tdmEnabled == enabled && m_tdmPair02 == pair02) return;
    m_tdmEnabled = enabled;
    m_tdmPair02 = pair02;
    m_selectedLane = -1;
    if (m_grid) m_grid->setSelectedLane(-1);
    if (m_detailWindow) m_detailWindow->hide();
    restartDetection();
}

void SpikePanel::onActivated() {
    if (m_hub && m_hub->isRunning() && !m_running) {
        startDetection();
    }
}

void SpikePanel::onDeactivated() {
    if (m_detailWindow) m_detailWindow->hide();
    if (!m_running) return;
    m_refreshTimer.stop();
    m_running = false;
    if (!stopWorker()) {
        setStatus(QStringLiteral("状态: 正在停止，请稍候"), "warn");
        return;
    }
    setStatus(QStringLiteral("状态: 已停止"), "warn");
}

void SpikePanel::shutdown() {
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveConfig();
    }
    m_refreshTimer.stop();
    m_running = false;
    if (m_detailWindow) m_detailWindow->hide();
    if (!stopWorker()) {
        // Never let the page be destroyed with a live thread reading its stop
        // flag and queue: join unconditionally.
        if (m_stopFlag) m_stopFlag->store(true);
        if (m_rawQueue) m_rawQueue->wakeAll();
        if (m_worker) {
            m_worker->wait();
            m_worker->deleteLater();
            m_worker = nullptr;
        }
    }
}

void SpikePanel::setWaveTheme(const QMap<QString, QString> &palette) {
    if (m_grid) m_grid->setThemePalette(palette);
    if (m_detailWindow) m_detailWindow->setWaveTheme(palette);
}

void SpikePanel::setStatus(const QString &text, const char *state) {
    setStatusLine(m_status, text, state);
}

void SpikePanel::scheduleSaveConfig() {
    if (m_loadingConfig) return;
    m_saveTimer.start();
}

void SpikePanel::loadConfig() {
    if (!m_cfgMgr) return;
    m_loadingConfig = true;
    const ConfigSection sec = m_cfgMgr->load().value(QStringLiteral("SpikePanel"));
    auto readInt = [&](const QString &key, int fallback) {
        bool ok = false;
        const int v = sec.value(key).toInt(&ok);
        return ok ? v : fallback;
    };
    auto readDouble = [&](const QString &key, double fallback) {
        bool ok = false;
        const double v = sec.value(key).toDouble(&ok);
        return ok ? v : fallback;
    };
    const double configuredGain = readDouble(QStringLiteral("input_gain"), 60.0);
    int gainIndex = m_gainCombo->findData(configuredGain);
    if (gainIndex < 0) {
        gainIndex = std::abs(configuredGain - 180.0) <
                            std::abs(configuredGain - 60.0)
                        ? m_gainCombo->findData(180.0)
                        : m_gainCombo->findData(60.0);
    }
    m_gainCombo->setCurrentIndex(qMax(0, gainIndex));
    const double inputGain =
        qMax(1.0, m_gainCombo->currentData().toDouble());
    const bool migrateThresholdUnits =
        sec.value(QStringLiteral("threshold_units")).trimmed() !=
        QStringLiteral("input_referred");
    const int mode = readInt(QStringLiteral("threshold_mode"), 0);
    m_threshModeCombo->setCurrentIndex(m_threshModeCombo->findData(mode));
    m_threshSpin->setSuffix(mode == 1 ? QStringLiteral(" µV") : QStringLiteral(" ×RMS"));
    double thresholdValue =
        readDouble(QStringLiteral("threshold_value"), mode == 1 ? 50.0 : 4.0);
    if (migrateThresholdUnits && mode == 1) {
        thresholdValue /= inputGain;
    }
    m_threshSpin->setValue(thresholdValue);
    const int pol = readInt(QStringLiteral("polarity"), 1);
    m_polarityCombo->setCurrentIndex(m_polarityCombo->findData(pol));
    m_hpSpin->setValue(readInt(QStringLiteral("highpass_hz"), 250));
    const int notch = readInt(QStringLiteral("notch_hz"), 0);
    m_notchCombo->setCurrentIndex(qMax(0, m_notchCombo->findData(notch)));
    m_preSpin->setValue(readDouble(QStringLiteral("pre_ms"), 0.4));
    m_postSpin->setValue(readDouble(QStringLiteral("post_ms"), 1.2));
    m_retainSpin->setValue(readInt(QStringLiteral("retain"), 200));
    m_yScaleSpin->setValue(readDouble(QStringLiteral("y_scale_uv"), 200.0));
    m_autoScaleCheck->setChecked(readInt(QStringLiteral("auto_scale"), 0) != 0);
    m_fadeCheck->setChecked(readInt(QStringLiteral("fade"), 1) != 0);
    m_columnsSpin->setValue(readInt(QStringLiteral("columns"), 0));
    m_detailYScaleUv =
        readDouble(QStringLiteral("detail_y_scale_uv"), m_yScaleSpin->value());
    m_detailAutoScale =
        readInt(QStringLiteral("detail_auto_scale"),
                m_autoScaleCheck->isChecked() ? 1 : 0) != 0;
    m_detailFade =
        readInt(QStringLiteral("detail_fade"),
                m_fadeCheck->isChecked() ? 1 : 0) != 0;
    m_detailOverlayCount =
        qBound(10, readInt(QStringLiteral("detail_overlay_count"), 100), 2000);
    if (m_detailWindow) {
        m_detailWindow->setDisplayOptions(
            m_detailYScaleUv, m_detailAutoScale, m_detailFade,
            m_detailOverlayCount);
    }
    m_adcThresholdOverridesUv = parseThresholdOverrides(
        sec.value(QStringLiteral("threshold_overrides_adc")));
    m_tdmThresholdOverridesUv = parseThresholdOverrides(
        sec.value(QStringLiteral("threshold_overrides_tdm")));
    if (migrateThresholdUnits) {
        for (auto it = m_adcThresholdOverridesUv.begin();
             it != m_adcThresholdOverridesUv.end(); ++it) {
            it.value() /= inputGain;
        }
        for (auto it = m_tdmThresholdOverridesUv.begin();
             it != m_tdmThresholdOverridesUv.end(); ++it) {
            it.value() /= inputGain;
        }
    }
    m_loadingConfig = false;
    if (migrateThresholdUnits) {
        saveConfig();
    }
}

void SpikePanel::saveConfig() const {
    if (!m_cfgMgr) return;
    ConfigMap cfg = m_cfgMgr->load();
    ConfigSection &sec = cfg[QStringLiteral("SpikePanel")];
    sec[QStringLiteral("input_gain")] =
        QString::number(m_gainCombo->currentData().toDouble(), 'f', 0);
    sec[QStringLiteral("threshold_units")] =
        QStringLiteral("input_referred");
    sec[QStringLiteral("threshold_mode")] = QString::number(m_threshModeCombo->currentData().toInt());
    sec[QStringLiteral("threshold_value")] = QString::number(m_threshSpin->value());
    sec[QStringLiteral("polarity")] = QString::number(m_polarityCombo->currentData().toInt());
    sec[QStringLiteral("highpass_hz")] = QString::number(m_hpSpin->value());
    sec[QStringLiteral("notch_hz")] = QString::number(m_notchCombo->currentData().toInt());
    sec[QStringLiteral("pre_ms")] = QString::number(m_preSpin->value());
    sec[QStringLiteral("post_ms")] = QString::number(m_postSpin->value());
    sec[QStringLiteral("retain")] = QString::number(m_retainSpin->value());
    sec[QStringLiteral("y_scale_uv")] = QString::number(m_yScaleSpin->value());
    sec[QStringLiteral("auto_scale")] = m_autoScaleCheck->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
    sec[QStringLiteral("fade")] = m_fadeCheck->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
    sec[QStringLiteral("columns")] = QString::number(m_columnsSpin->value());
    sec[QStringLiteral("detail_y_scale_uv")] =
        QString::number(m_detailYScaleUv);
    sec[QStringLiteral("detail_auto_scale")] =
        m_detailAutoScale ? QStringLiteral("1") : QStringLiteral("0");
    sec[QStringLiteral("detail_fade")] =
        m_detailFade ? QStringLiteral("1") : QStringLiteral("0");
    sec[QStringLiteral("detail_overlay_count")] =
        QString::number(m_detailOverlayCount);
    sec[QStringLiteral("threshold_overrides_adc")] =
        serializeThresholdOverrides(m_adcThresholdOverridesUv);
    sec[QStringLiteral("threshold_overrides_tdm")] =
        serializeThresholdOverrides(m_tdmThresholdOverridesUv);
    m_cfgMgr->save(cfg);
}

}  // namespace ccv2
