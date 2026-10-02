#include "ui/spike_panel.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
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
#include "io/spike_event_exporter.h"
#include "service/session_hub.h"
#include "service/spike_offline_analysis.h"
#include "ui/widgets/spike_analysis_controls.h"
#include "ui/widgets/spike_archive_browser.h"
#include "ui/widgets/spike_rule_editor.h"
#include "ui/widgets/spike_detail_window.h"
#include "ui/widgets/spike_grid_view.h"
#include "ui/widgets/spike_sorting_widget.h"

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
    m_store.setAnnotationCallback([this](const QVector<SpikeEvent> &events) { m_archive.annotate(events); });
    connect(&m_analysisToolsTimer, &QTimer::timeout, this, &SpikePanel::updateAnalysisTools);
    m_analysisToolsTimer.setInterval(250);
    m_analysisToolsTimer.start();
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
    m_threshModeCombo->addItem(QStringLiteral("绝对 µV"), 1);
    m_threshModeCombo->setMinimumWidth(84);
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
    m_btnPause->setObjectName(QStringLiteral("spikePauseDisplay"));
    m_btnPause->setToolTip(QStringLiteral("仅暂停波形刷新；后台神经分析、采集与录制继续运行"));
    m_btnPause->setProperty("variant", "secondary");
    m_btnPause->setCheckable(true);
    vbar->addWidget(m_btnPause);
    m_btnClear = new QPushButton(QStringLiteral("清空留存"));
    m_btnClear->setProperty("variant", "secondary");
    m_btnClear->setToolTip(QStringLiteral("清空留存并重启分析统计区间；不会停止采集或录制"));
    vbar->addWidget(m_btnClear);
    auto *sortingButton = new QPushButton(QStringLiteral("候选分选"));
    sortingButton->setObjectName(QStringLiteral("spikeSortingWorkspace"));
    sortingButton->setProperty("variant", "secondary");
    sortingButton->setToolTip(QStringLiteral("在当前会话内查看特征、源时间 raster、ISI，并手动标记候选单元；仅限留存窗口"));
    vbar->addWidget(sortingButton);
    m_btnExport = new QPushButton(QStringLiteral("导出选中电极"));
    m_btnExport->setObjectName(QStringLiteral("spikeExportLane"));
    m_btnExport->setProperty("variant", "secondary");
    m_btnExport->setEnabled(false);
    m_btnExport->setToolTip(QStringLiteral("先点击一个电极，再导出其留存事件、波形、候选标签及分析参数 JSON；不是完整事件录制"));
    vbar->addWidget(m_btnExport);
    vbar->addStretch(1);
    m_status = new QLabel(QStringLiteral("状态: 未开始"));
    m_status->setObjectName(QStringLiteral("spikeAnalysisStatus"));
    m_status->setProperty("role", "status-line");
    m_status->setProperty("state", "warn");
    m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_status->setWordWrap(true);
    m_status->setMinimumWidth(0);
    root->addWidget(viewOpts);
    auto *statusControls = new QWidget;
    statusControls->setProperty("role", "page-control-panel");
    auto *statusRow = new QHBoxLayout(statusControls);
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    statusRow->addWidget(m_status, 1);
    auto *detailsButton = new QPushButton(QStringLiteral("分析详情 ▾"));
    detailsButton->setObjectName(QStringLiteral("spikeAnalysisDetails"));
    detailsButton->setProperty("variant", "secondary");
    detailsButton->setCheckable(true);
    statusRow->addWidget(detailsButton);
    root->addWidget(statusControls);

    m_qualitySummary = new QLabel(QStringLiteral("分析随会话持续运行；切页和暂停显示不停止分析"));
    m_qualitySummary->setObjectName(QStringLiteral("spikeCoverageSummary"));
    m_qualitySummary->setProperty("role", "caption");
    m_qualitySummary->setWordWrap(true);
    root->addWidget(m_qualitySummary);

    m_qualityStatus = new QLabel(QStringLiteral("分析随采集或回放自动启动；切换页面不停止分析。频率按已分析的源时间计算"));
    m_qualityStatus->setObjectName(QStringLiteral("spikeAnalysisQuality"));
    m_qualityStatus->setProperty("role", "caption");
    m_qualityStatus->setWordWrap(true);
    auto *details = new QWidget;
    details->setObjectName(QStringLiteral("spikeAnalysisDetailsPanel"));
    details->setProperty("role", "plot-area");
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(10, 8, 10, 8);
    detailsLayout->addWidget(m_qualityStatus);
    root->addWidget(details);
    details->hide();
    connect(detailsButton, &QPushButton::toggled, this, [details, detailsButton](bool expanded) {
        details->setVisible(expanded);
        detailsButton->setText(expanded ? QStringLiteral("收起详情 ▴") : QStringLiteral("分析详情 ▾"));
    });

    auto *archiveToolsButton = new QPushButton(QStringLiteral("事件档案 · 离线分析 · 持续分类 ▾"));
    archiveToolsButton->setObjectName(QStringLiteral("spikeArchiveTools"));
    archiveToolsButton->setCheckable(true);
    root->addWidget(archiveToolsButton, 0, Qt::AlignLeft);
    m_analysisControls = new SpikeAnalysisControls(this);
    root->addWidget(m_analysisControls);
    m_analysisControls->hide();
    connect(archiveToolsButton, &QPushButton::toggled, m_analysisControls, &QWidget::setVisible);
    m_archiveBrowser = new SpikeArchiveBrowser(this);
    m_ruleEditor = new SpikeRuleEditor(this);
    connect(m_analysisControls, &SpikeAnalysisControls::startArchiveRequested, this, [this](const QString &path) {
        QString error; if (!startEventArchive(path, &error)) setStatus(error, "error");
    });
    connect(m_analysisControls, &SpikeAnalysisControls::stopArchiveRequested, this, &SpikePanel::stopEventArchive);
    connect(m_analysisControls, &SpikeAnalysisControls::openArchiveRequested, this, [this](const QString &path) {
        m_archiveBrowser->openArchive(path); m_archiveBrowser->show(); m_archiveBrowser->raise();
    });
    connect(m_analysisControls, &SpikeAnalysisControls::ruleEditorRequested, this, [this] {
        m_ruleEditor->setLane(qMax(0, m_selectedLane));
        const auto applied=m_worker ? m_worker->appliedRuleSet() : SpikeRuleSet::Snapshot{};
        m_ruleEditor->setProperty("appliedRevision",QVariant::fromValue(applied ? applied->revision() : quint64(0)));
        m_ruleEditor->setAppliedRules(applied,m_worker ? QString{} : QStringLiteral("检测未运行；会话中的待应用规则尚无当前服务确认"));
        m_ruleEditor->setRequestedRules(m_rules);
        captureRuleContext(m_ruleEditor->lane());
        m_ruleEditor->show(); m_ruleEditor->raise();
    });
    connect(m_ruleEditor, &SpikeRuleEditor::captureRequested, this, &SpikePanel::captureRuleContext);
    connect(m_ruleEditor, &SpikeRuleEditor::applyRulesRequested, this, [this](const QVector<SpikeElectrodeRules> &rules) {
        QString error; if (!applyCandidateRules(rules, &error)) m_ruleEditor->setServiceStatus(error);
    });
    connect(m_analysisControls, &SpikeAnalysisControls::analyzeRecordingRequested, this,
            [this](const QString &input, const QString &output) {
        QString error; if (!startOfflineAnalysis(input,output,&error))
            m_analysisControls->setOfflineProgress(0,0,error,false);
    });
    connect(m_analysisControls, &SpikeAnalysisControls::cancelAnalysisRequested, this, [this] {
        if (m_offline) m_offline->requestCancel();
    });

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
    gridHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
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
    m_sortingWindow = new SpikeSortingWidget(&m_store, this);
    connect(sortingButton, &QPushButton::clicked, this, [this]() {
        m_sortingWindow->setLane(qMax(0, m_selectedLane));
        m_sortingWindow->refreshData();
        m_sortingWindow->show();
        m_sortingWindow->raise();
        m_sortingWindow->activateWindow();
    });
    connect(m_btnExport, &QPushButton::clicked, this, &SpikePanel::exportSelectedLane);
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
        refresh();
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
    m_btnExport->setEnabled(true);
    if (m_sortingWindow && m_sortingWindow->isVisible()) m_sortingWindow->setLane(lane);

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

bool SpikePanel::configureAnalysisStore(const SpikeDetectConfig &cfg) {
    const int lanes = laneCount();
    const int len = cfg.preSamples + cfg.postSamples + 1;
    const int requestedRetention = m_retainSpin ? m_retainSpin->value() : 200;
    if (!m_store.configure(lanes, len, requestedRetention)) {
        m_store.resetTimeline(m_hub ? m_hub->timelineEpoch() : 0);
        resetViewState();
        setStatus(QStringLiteral("状态: Spike 留存内存不足，请缩短窗口或降低采样率"), "error");
        return false;
    }
    m_store.resetTimeline(m_hub ? m_hub->timelineEpoch() : 0);
    if (m_retainSpin && m_store.capacity() < requestedRetention) {
        const QSignalBlocker blocker(m_retainSpin);
        m_retainSpin->setMinimum(1);
        m_retainSpin->setValue(m_store.capacity());
        m_retainSpin->setToolTip(QStringLiteral("已按 128 MiB 留存内存上限调整条数"));
        scheduleSaveConfig();
    }
    m_rates.fill(0.0, lanes);
    m_analysisSampleRate = cfg.proc.sampleRate;
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

    resetViewState();
    m_analysisProvenance = {};
    return true;
}

void SpikePanel::startDetection() {
    if (m_shutdown || m_running || !m_hub || m_drainedEpoch == m_hub->timelineEpoch()) return;
    if (!m_hub->isRunning()) {
        setStatus(QStringLiteral("状态: 会话未运行 — 请先在顶部开始接收或加载 BIN"), "warn");
        return;
    }
    if (!stopWorker()) {
        setStatus(QStringLiteral("状态: 上一轮检测线程尚未退出，请稍候重试"), "error");
        return;
    }

    const SpikeDetectConfig cfg = currentConfig();
    if (!configureAnalysisStore(cfg)) return;
    const int lanes = laneCount();

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
    const bool negative = cfg.proc.negativePolarity;
    const QMap<int, double> &overrides = activeThresholdOverrides();
    for (int lane = 0; lane < lanes; ++lane) {
        const auto it = overrides.constFind(thresholdKeyForLane(lane));
        if (it == overrides.cend()) continue;
        const double thresholdV =
            (negative ? -it.value() : it.value()) * 1e-6;
        m_worker->setLaneThresholdOverride(lane, true, thresholdV);
    }
    m_archive.attach(m_worker);
    m_worker->setRuleSet(m_rules);
    m_worker->start();

    m_analysisError.clear();
    m_running = true;
    publishAnalysisMetadata(true);
    if (m_viewActive) m_refreshTimer.start();
    setStatus(QStringLiteral("状态: 检测中 (%1 电极，输入等效 %2×)")
                  .arg(lanes)
                  .arg(cfg.inputGain, 0, 'f', 0),
              "ok");
}

bool SpikePanel::stopWorker() {
    if (m_worker) {
        SpikeAnalysisQuality quality; m_store.snapshotAnalysis(nullptr,&quality);
        m_store.markAnalysisIncomplete(quality.epoch);
    }
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
    delete m_worker;
    m_worker = nullptr;
    QJsonObject integrity = spikeQualityJson(m_store);
    integrity["end_reason"] = "analysis_interrupted_or_reconfigured";
    integrity["source_provenance"] = m_replayAnalysisMetadata.value("source_provenance");
    m_archive.finish(SpikeArchiveState::Drained, integrity);
    return true;
}

void SpikePanel::restartDetection() {
    if (m_loadingConfig || m_shutdown) return;
    if (!m_running) {
        // Explicit detector/mapping edits invalidate frozen results too: old
        // ADC data must never be shown under newly selected TDM identities.
        if (!stopWorker()) {
            setStatus(QStringLiteral("状态: 检测线程尚未退出，不能更新分析配置"), "error");
            return;
        }
        if (configureAnalysisStore(currentConfig())) {
            m_analysisError.clear();
            publishAnalysisMetadata(true);
            refresh();
        }
        return;
    }
    m_refreshTimer.stop();
    m_running = false;
    if (!stopWorker()) {
        setStatus(QStringLiteral("状态: 检测线程停止超时，请稍候重试"), "error");
        return;
    }
    startDetection();
}

void SpikePanel::resetViewState() {
    m_rates.fill(0.0, m_store.channels());
    if (m_grid) {
        m_grid->clearTraces();
        m_grid->setRates(m_rates);
    }
    if (m_detailWindow) m_detailWindow->clearTraces();
}

void SpikePanel::clearAll() {
    // A user-requested reset starts a coherent detector/exposure interval.
    // Clearing the store under an in-flight batch would count old crossings
    // against a new exposure denominator.
    if (m_running) {
        restartDetection();
    } else {
        m_store.clear();
    }
    resetViewState();
    refresh();
}

void SpikePanel::refresh() {
    if (m_viewActive && !m_paused) {
        m_grid->pullNewSnippets();
        if (m_detailWindow) m_detailWindow->pullNewSnippets();
    }

    QVector<qint64> totals;
    QVector<qint64> observed;
    QVector<double> thresholds;
    QVector<double> rms;
    SpikeAnalysisQuality quality;
    m_store.snapshotAnalysis(&observed, &quality, &totals, &thresholds, &rms);
    const bool coverageIssue = quality.missingSourceFrames > 0 || quality.queueDroppedFrames > 0 ||
        quality.invalidFrames > 0 || quality.unverifiedFrames > 0 || quality.discontinuities > 0 ||
        quality.boundaryExcludedEvents > 0 || quality.stoppedEarly;

    m_rates.fill(0.0, totals.size());
    qint64 grandTotal = 0;
    qint64 minSamples = observed.isEmpty() ? 0 : observed.first();
    qint64 maxSamples = 0;
    int activeLanes = 0;
    for (int lane = 0; lane < totals.size(); ++lane) {
        const qint64 samples = observed.value(lane);
        // A mean over actually analyzed SOURCE samples. Rendering cadence,
        // replay speed and pauses cannot change this denominator. Never turn
        // a period with no new samples into artificial zero-rate evidence.
        m_rates[lane] = samples > 0 && m_analysisSampleRate > 0.0
                            ? static_cast<double>(totals[lane]) * m_analysisSampleRate / samples
                            : 0.0;
        grandTotal += totals[lane];
        minSamples = qMin(minSamples, samples);
        maxSamples = qMax(maxSamples, samples);
        if (m_rates[lane] >= 1.0) ++activeLanes;
    }
    m_grid->setRates(m_rates);
    m_grid->setThresholds(thresholds);
    if (m_detailWindow && m_detailWindow->isVisible() && !m_paused) {
        m_detailWindow->updateData(totals, thresholds, rms, m_rates);
    }

    QString stateText;
    if (!m_running) {
        stateText = maxSamples > 0 ? QStringLiteral("分析已停止 · 保留结果")
                                  : QStringLiteral("等待采集或回放");
    } else if (m_hub && (m_hub->state() == SessionHub::State::ReplayPaused ||
                         m_hub->state() == SessionHub::State::ReplayReady)) {
        stateText = QStringLiteral("回放暂停/就绪 · 分析保持");
    } else {
        stateText = QStringLiteral("连续分析中");
    }
    if (m_paused) stateText += QStringLiteral(" · 显示暂停");
    if (!m_running && !m_analysisError.isEmpty()) {
        setStatus(m_analysisError, "error");
    } else {
        setStatus(QStringLiteral("%1 | 完整窗事件 %2 | 活跃 %3/%4")
                      .arg(stateText).arg(grandTotal).arg(activeLanes).arg(totals.size()),
                  coverageIssue ? "warn" : (m_running ? "ok" : "warn"));
    }

    const double fs = qMax(1.0, m_analysisSampleRate);
    const QString exposure = minSamples == maxSamples
        ? QStringLiteral("%1 s/电极").arg(maxSamples / fs, 0, 'f', 3)
        : QStringLiteral("%1–%2 s/电极").arg(minSamples / fs, 0, 'f', 3)
                                             .arg(maxSamples / fs, 0, 'f', 3);
    const QString frameRange = quality.firstSourceFrame >= 0
        ? QStringLiteral("%1–%2").arg(quality.firstSourceFrame).arg(quality.lastSourceFrame)
        : QStringLiteral("无");
    const QString scope = m_store.channels() == kChannelsTotal * 2
        ? QStringLiteral("TDM 当前相位对 512/1024 电极") : QStringLiteral("256 ADC 通道");
    QString qualityText = QStringLiteral("%1 | 已分析 %2 | 源帧 %3 | epoch %4 | Hz = 累计事件 / 已分析源时间")
        .arg(scope).arg(exposure).arg(frameRange).arg(quality.epoch);
    if (coverageIssue) {
        qualityText += QStringLiteral(" | 覆盖不完整: 缺失 %1 帧, 队列丢失 %2 帧, 无效 %3 帧, 中断 %4, 未验证有效性 %5 帧")
            .arg(quality.missingSourceFrames).arg(quality.queueDroppedFrames)
            .arg(quality.invalidFrames).arg(quality.discontinuities).arg(quality.unverifiedFrames);
        qualityText += QStringLiteral("; 边界排除 %1").arg(quality.boundaryExcludedEvents);
        if (quality.stoppedEarly) qualityText += QStringLiteral("; 停止时未保证尾部分析完成");
    } else {
        qualityText += QStringLiteral(" | 此分析区间未发现缺口");
    }
    if (quality.pendingWindowEvents > 0) {
        qualityText += QStringLiteral(" | 待完成后窗 %1（等待后续源样本）")
            .arg(quality.pendingWindowEvents);
    }
    if (!m_replayProvenanceNote.isEmpty()) qualityText += QStringLiteral("\n") + m_replayProvenanceNote;
    m_qualityStatus->setText(qualityText);
    QStringList alerts;
    if (quality.missingSourceFrames > 0 || quality.queueDroppedFrames > 0 ||
        quality.invalidFrames > 0 || quality.discontinuities > 0) alerts << QStringLiteral("存在缺口或无效帧");
    if (quality.unverifiedFrames > 0) alerts << QStringLiteral("来源有效性未验证");
    if (quality.stoppedEarly) alerts << QStringLiteral("停止尾部未保证完整");
    if (quality.boundaryExcludedEvents > 0) alerts << QStringLiteral("已排除边界不完整窗口");
    if (quality.pendingWindowEvents > 0) alerts << QStringLiteral("%1 个窗口等待后续样本").arg(quality.pendingWindowEvents);
    const QJsonObject source = m_replayAnalysisMetadata.value("source_provenance").toObject();
    if (!source.isEmpty() && !source.value("integrity_complete").toBool())
        alerts << QStringLiteral("来源完整性未证实");
    if (source.value("ignored_tail_bytes").toVariant().toLongLong() > 0)
        alerts << QStringLiteral("文件尾有未读取字节");
    if (!m_replayProvenanceNote.isEmpty()) alerts << QStringLiteral("回放采用当前分析设置");
    m_qualitySummary->setText(QStringLiteral("%1 · 已分析 %2 · %3")
        .arg(scope, exposure, alerts.isEmpty() ? QStringLiteral("当前区间未发现已知缺口") : alerts.join(QStringLiteral(" · "))));
    m_qualitySummary->setToolTip(qualityText);
    m_qualityStatus->setToolTip(QStringLiteral("只覆盖本轮检测实际处理的样本；计数为有完整前后波形窗的阈值事件，边界处不完整的窗不会计入。参数调整、清空和跳转会开启新统计区间。留存波形有容量上限，不等于完整事件档案。切页和暂停显示不会重置分析"));
}

void SpikePanel::publishAnalysisMetadata(bool newInterval) {
    if (m_loadingConfig || !m_hub) return;
    const QJsonObject current = spikeAnalysisMetadata(
        currentConfig(), m_hub->referenceMode(), activeThresholdOverrides());
    m_hub->setAnalysisMetadata(current);
    if (!m_running && !newInterval) return; // preserve stopped results' provenance
    if (newInterval || m_analysisProvenance.isEmpty()) {
        m_analysisProvenance = current;
        m_analysisProvenance["configuration_changes"] = QJsonArray{};
        m_analysisProvenance["configuration_changed"] = false;
        m_analysisProvenance["history_truncated"] = false;
        m_analysisProvenance["latest_requested_configuration"] = current;
        m_analysisProvenance["change_time_semantics"] =
            "requested_source_frame_not_atomic_detector_application";
    } else if (m_analysisProvenance.value("latest_requested_configuration").toObject() != current) {
        QJsonArray changes = m_analysisProvenance.value("configuration_changes").toArray();
        if (changes.size() < 256) {
            changes.append(QJsonObject{
                {"requested_source_frame", QString::number(m_hub->currentFrameIndex())},
                {"configuration", current}});
            m_analysisProvenance["configuration_changes"] = changes;
        } else {
            m_analysisProvenance["history_truncated"] = true;
        }
        m_analysisProvenance["configuration_changed"] = true;
        m_analysisProvenance["latest_requested_configuration"] = current;
    }
}

void SpikePanel::exportSelectedLane() {
    const int lane = m_selectedLane;
    if (lane < 0 || lane >= m_store.channels()) return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出选中电极的留存事件窗口（非完整录制）"),
        QStringLiteral("spike_lane_%1_retained.json").arg(lane),
        QStringLiteral("Spike 事件 JSON (*.json)"));
    if (path.isEmpty()) return;
    // Snapshot after closing the dialog, then export one bounded, atomically
    // paired set of identities/waveforms/labels/coverage and its provenance.
    if (lane != m_selectedLane || lane >= m_store.channels()) return;
    const SpikeLaneSnapshot snapshot = m_store.snapshotLane(lane);
    QJsonObject metadata = m_analysisProvenance;
    if (!m_replayAnalysisMetadata.isEmpty()) {
        metadata["recorded_source_analysis"] = m_replayAnalysisMetadata;
        metadata["replay_configuration_policy"] = "current_settings_not_automatic_original_reproduction";
    }
    QString error;
    if (!exportSpikeLaneJson(path, snapshot, metadata, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出未完成"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("留存事件已导出"),
        QStringLiteral("已导出电极 %1 的 %2 条留存事件、波形和分析参数。\n这是有容量上限的窗口，不是完整事件档案。\n%3")
            .arg(lane).arg(snapshot.events.size()).arg(path));
}

void SpikePanel::syncSessionState() {
    if (m_shutdown || !m_hub) return;
    if (m_hub->state() == SessionHub::State::Stopping) return; // producer drain barrier follows
    if (m_hub->isRunning()) {
        if (!m_hub->isReplaying()) {
            m_replayAnalysisMetadata = {};
            m_replayProvenanceNote.clear();
        }
        if (!m_running) startDetection();
        if (m_viewActive) refresh();
        return;
    }
    if (m_worker) {
        // Session stop can discard queued/in-flight data. Keep the completed
        // snapshot, explicitly marked partial, before the hub invalidates its
        // epoch. A new start/seek/configuration owns its own fresh interval.
        SpikeAnalysisQuality quality;
        m_store.snapshotAnalysis(nullptr, &quality);
        m_store.markAnalysisIncomplete(quality.epoch);
        m_running = false;
        if (!stopWorker()) {
            // Stopping can come from SessionHub's destructor. It is unsafe
            // to return while the thread still holds its atomic pointers.
            setStatus(QStringLiteral("状态: 正在等待检测线程安全退出"), "warn");
            m_worker->wait();
            delete m_worker;
            m_worker = nullptr;
        }
    }
    m_refreshTimer.stop();
    refresh();
}

void SpikePanel::setSessionHub(SessionHub *hub) {
    if (m_hub == hub || m_shutdown) return;
    if (m_hub) {
        // Join before changing the hub: the worker holds pointers to the old
        // hub's atomic timeline/reference counters.
        if (m_worker) {
            SpikeAnalysisQuality quality;
            m_store.snapshotAnalysis(nullptr, &quality);
            m_store.markAnalysisIncomplete(quality.epoch);
        }
        m_running = false;
        if (!stopWorker()) {
            setStatus(QStringLiteral("状态: 检测线程尚未退出，不能切换会话"), "error");
            return;
        }
        disconnect(m_hub, nullptr, this, nullptr);
        if (m_hub->tdmContext()) disconnect(m_hub->tdmContext(), nullptr, this, nullptr);
    }
    m_refreshTimer.stop();
    m_hub = hub;
    if (!m_hub) return;
    if (TdmContext *tdm = m_hub->tdmContext()) {
        applyTdmState(tdm->enabled(), tdm->pair02());
        connect(tdm, &TdmContext::changed, this, &SpikePanel::applyTdmState);
    }
    connect(m_hub, &SessionHub::sourceDrained, this, &SpikePanel::drainWorker);
    connect(m_hub, &SessionHub::stateChanged, this, [this](SessionHub::State) {
        syncSessionState();
    });
    connect(m_hub, &SessionHub::referenceModeChanged, this, [this](int) {
        // A different reference changes the filter input; don't mix its
        // transients, thresholds and counts into the preceding interval.
        restartDetection();
        publishAnalysisMetadata();
    });
    connect(m_hub, &SessionHub::replayAnalysisMetadataAvailable, this,
            [this](const QJsonObject &metadata) {
        m_replayAnalysisMetadata = metadata;
        if (metadata.value("initial_configuration").toObject().isEmpty() &&
            metadata.value("algorithm").toString().isEmpty()) {
            m_replayProvenanceNote = QStringLiteral("回放来源未记录神经分析参数；使用当前设置重新分析");
        } else if (metadata.value("configuration_changed").toBool() ||
                   metadata.value("history_truncated").toBool() ||
                   !metadata.value("configuration_changes").toArray().isEmpty()) {
            m_replayProvenanceNote = QStringLiteral("原录制含分析参数变更；当前回放使用当前设置，未逐帧复现原分析配置。原始参数随事件导出保留");
        } else {
            m_replayProvenanceNote = QStringLiteral("原录制分析参数已保留；当前回放使用当前设置重新分析，不保证与原检测结果相同");
        }
        const QJsonObject source = metadata.value("source_provenance").toObject();
        const qint64 ignoredTail = source.value("ignored_tail_bytes").toVariant().toLongLong();
        if (!source.isEmpty() && (!source.value("integrity_complete").toBool() ||
                                  !source.value("frame_validity_known").toBool() || ignoredTail > 0)) {
            m_replayProvenanceNote += QStringLiteral("\n来源完整性未证实: 清单 %1，逐帧有效性 %2，忽略文件尾 %3 字节")
                .arg(source.value("has_manifest").toBool() ? QStringLiteral("有") : QStringLiteral("无"))
                .arg(source.value("frame_validity_known").toBool() ? QStringLiteral("已记录") : QStringLiteral("未知"))
                .arg(ignoredTail);
        }
        if (m_viewActive) refresh();
    });
    connect(m_hub, &SessionHub::timelineReset, this, [this](quint64 epoch, qint64) {
        // A seek/reset invalidates retained events and exposure together. The
        // worker checks the epoch before publishing, so old work cannot leak.
        if (m_archive.status().state == SpikeArchiveState::Running) {
            QJsonObject integrity = spikeQualityJson(m_store);
            integrity["end_reason"] = "timeline_changed";
            integrity["stopped_early"] = true;
            m_archive.finish(SpikeArchiveState::Drained, integrity);
        }
        m_drainedEpoch = std::numeric_limits<quint64>::max();
        m_store.resetTimeline(epoch);
        resetViewState();
        if (!m_worker && m_hub && m_hub->isRunning()) startDetection();
        if (m_viewActive) refresh();
    });
    publishAnalysisMetadata();
    syncSessionState();
}

void SpikePanel::applyTdmState(bool enabled, bool pair02) {
    if (m_tdmEnabled == enabled && m_tdmPair02 == pair02) return;
    m_tdmEnabled = enabled;
    m_tdmPair02 = pair02;
    m_selectedLane = -1;
    if (m_btnExport) m_btnExport->setEnabled(false);
    if (m_grid) m_grid->setSelectedLane(-1);
    if (m_detailWindow) m_detailWindow->hide();
    if (m_sortingWindow) m_sortingWindow->hide();
    restartDetection();
    publishAnalysisMetadata();
}

void SpikePanel::onActivated() {
    if (m_shutdown) return;
    m_viewActive = true;
    syncSessionState();
    if (m_running) m_refreshTimer.start();
    refresh();
}

void SpikePanel::onDeactivated() {
    m_viewActive = false;
    if (m_detailWindow) m_detailWindow->hide();
    if (m_sortingWindow) m_sortingWindow->hide();
    // Only the view sleeps. The bounded store and session subscriber continue
    // collecting events, even when the spike tab has never been opened.
    m_refreshTimer.stop();
}

void SpikePanel::shutdown() {
    m_shutdown = true;
    m_analysisToolsTimer.stop();
    if (m_offline) { m_offline->requestCancel(); m_offline->wait(); delete m_offline; m_offline = nullptr; }
    if (m_archiveBrowser) m_archiveBrowser->hide();
    if (m_ruleEditor) m_ruleEditor->hide();
    m_viewActive = false;
    if (m_worker) {
        SpikeAnalysisQuality quality;
        m_store.snapshotAnalysis(nullptr, &quality);
        m_store.markAnalysisIncomplete(quality.epoch);
    }
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveConfig();
    }
    m_refreshTimer.stop();
    m_running = false;
    if (m_detailWindow) m_detailWindow->hide();
    if (m_sortingWindow) m_sortingWindow->hide();
    if (!stopWorker()) {
        // Never let the page be destroyed with a live thread reading its stop
        // flag and queue: join unconditionally.
        if (m_stopFlag) m_stopFlag->store(true);
        if (m_rawQueue) m_rawQueue->wakeAll();
        if (m_worker) {
            m_worker->wait();
            delete m_worker;
            m_worker = nullptr;
        }
    }
}

void SpikePanel::setWaveTheme(const QMap<QString, QString> &palette) {
    m_wavePalette = palette;
    if (m_analysisControls) m_analysisControls->setWaveTheme(palette);
    if (m_archiveBrowser) m_archiveBrowser->setWaveTheme(palette);
    if (m_ruleEditor) m_ruleEditor->setWaveTheme(palette);
    if (m_grid) m_grid->setThemePalette(palette);
    if (m_detailWindow) m_detailWindow->setWaveTheme(palette);
    if (m_sortingWindow) m_sortingWindow->setWaveTheme(palette);
}

bool SpikePanel::startEventArchive(const QString &directory, QString *error) {
    if (!m_worker || !m_running || !m_worker->isRunning()) {
        if (error) *error = QStringLiteral("事件档案需要正在运行的检测；可先加载回放并保持暂停");
        return false;
    }
    QJsonObject metadata = m_analysisProvenance;
    metadata["source_provenance"] = m_replayAnalysisMetadata.value("source_provenance");
    metadata["archive_requested_source_frame"] = QString::number(m_hub ? m_hub->currentFrameIndex() : 0);
    const bool ok = m_archive.start(directory,metadata,error);
    updateAnalysisTools(); return ok;
}
void SpikePanel::stopEventArchive() {
    QJsonObject integrity = spikeQualityJson(m_store);
    integrity["end_reason"] = "user_stopped_archive_at_emission_boundary";
    integrity["source_provenance"] = m_replayAnalysisMetadata.value("source_provenance");
    m_archive.finish(SpikeArchiveState::Drained,integrity,false);
    updateAnalysisTools();
}
void SpikePanel::drainWorker(quint64 epoch, bool eof, bool cleanStop) {
    if (!m_worker || !m_hub || epoch != m_hub->timelineEpoch()) return;
    m_hub->detachSubscriberForDrain(m_rawQueue);
    m_worker->requestDrain();
    m_worker->wait();
    const bool drained = m_worker->drained();
    if (!drained || !cleanStop) m_store.markAnalysisIncomplete(epoch);
    m_running = false;
    m_drainedEpoch = epoch;
    delete m_worker; m_worker = nullptr;
    QJsonObject integrity = spikeQualityJson(m_store);
    integrity["source_provenance"] = m_replayAnalysisMetadata.value("source_provenance");
    integrity["end_reason"] = eof ? "source_eof" : "source_stopped";
    integrity["producer_detector_drained"] = drained;
    integrity["source_stop_clean"] = cleanStop;
    m_archive.finish(eof && drained && cleanStop ? SpikeArchiveState::Completed : SpikeArchiveState::Drained,integrity);
    updateAnalysisTools(); refresh();
}
bool SpikePanel::applyCandidateRules(const QVector<SpikeElectrodeRules> &definitions, QString *error) {
    auto next = SpikeRuleSet::create(m_nextRuleRevision,definitions,error);
    if (!next) return false;
    if (m_worker && !m_worker->setRuleSet(next)) {
        if (error) *error = QStringLiteral("处理服务拒绝规则版本"); return false;
    }
    ++m_nextRuleRevision; m_rules = std::move(next);
    m_ruleEditor->setRequestedRules(m_rules);
    m_ruleEditor->setServiceStatus(m_worker
        ? QStringLiteral("规则已提交；将在下一个处理块边界用于未来事件，等待实际应用确认")
        : QStringLiteral("规则已保存于本次应用会话；下一次检测时生效。可导出 JSON 跨重启保留"));
    updateAnalysisTools(); return true;
}
void SpikePanel::captureRuleContext(int lane) {
    if (!m_worker || lane < 0 || lane >= m_worker->laneCount()) {
        m_ruleEditor->setServiceStatus(QStringLiteral("请先运行检测，再捕获正在处理的通道")); return;
    }
    quint64 revision=0;
    const SpikeRuleContext context = m_worker->appliedRuleContext(lane,&revision);
    const auto snapshot = m_store.snapshotLane(lane);
    QVector<SpikeArchiveRecord> records;
    for (int i=0; i<snapshot.events.size(); ++i) {
        const auto &event = snapshot.events[i];
        if (event.runId != m_worker->runId() || event.detectorRevision != revision) continue;
        records.push_back({event,snapshot.waveforms.mid(i*snapshot.snippetLength,snapshot.snippetLength)});
    }
    m_ruleEditor->setCapturedContext(context,records,QStringLiteral("仅捕获当前实际检测版本 %1 的留存波形；应用只影响未来事件").arg(revision));
}
bool SpikePanel::startOfflineAnalysis(const QString &input, const QString &output, QString *error) {
    if (m_offline && m_offline->isRunning()) {
        if (error) *error = QStringLiteral("请先完成或取消当前离线任务"); return false;
    }
    if (m_offline) { delete m_offline; m_offline=nullptr; }
    SpikeOfflineAnalysisRequest request;
    request.inputPath=input; request.outputDirectory=output;
    request.config=currentConfig(); request.referenceMode=m_hub ? m_hub->referenceMode() : 0;
    request.thresholdOverridesUvByElectrode=activeThresholdOverrides(); request.rules=m_rules;
    m_offline = new SpikeOfflineAnalysisJob(request,this);
    connect(m_offline,&SpikeOfflineAnalysisJob::progressChanged,this,&SpikePanel::updateAnalysisTools);
    connect(m_offline,&QThread::finished,this,&SpikePanel::updateAnalysisTools);
    m_offline->start(); updateAnalysisTools(); return true;
}
void SpikePanel::updateAnalysisTools() {
    if (!m_analysisControls) return;
    m_analysisControls->setArchiveStatus(m_archive.status(),m_archive.directory());
    if (m_offline) {
        const auto s=m_offline->status();
        const QString coverage = s.sourceIntegrity.value("incomplete_coverage").toBool()
            ? QStringLiteral(" · 来源或分析覆盖存在限制") : QString{};
        const QString text = QStringLiteral("档案状态 %1 · %2 事件 · %3%4%5")
            .arg(s.finalizing ? QStringLiteral("正在完成已接收尾部，不能取消") : spikeArchiveStateName(s.state)).arg(s.archive.writtenEvents)
            .arg(s.error, s.warning.isEmpty() ? QString{} : QStringLiteral(" · ")+s.warning, coverage);
        m_analysisControls->setOfflineProgress(s.processedFrames,s.totalFrames,text,m_offline->isRunning() && s.cancellable, m_offline->isRunning() && s.finalizing);
    }
    if (m_ruleEditor && m_worker) {
        const auto applied=m_worker->appliedRuleSet();
        const auto displayed=m_ruleEditor->property("appliedRevision").toULongLong();
        const quint64 revision=applied ? applied->revision() : 0;
        if (revision != displayed) {
            m_ruleEditor->setProperty("appliedRevision",QVariant::fromValue(revision));
            m_ruleEditor->setAppliedRules(applied,QStringLiteral("服务已在处理边界应用规则版本 %1").arg(revision));
        }
    }
}

void SpikePanel::setStatus(const QString &text, const char *state) {
    if (qstrcmp(state, "error") == 0) m_analysisError = text;
    setStatusLine(m_status, text, state);
}

void SpikePanel::scheduleSaveConfig() {
    if (m_loadingConfig) return;
    publishAnalysisMetadata();
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
