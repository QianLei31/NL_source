#include "ui/widgets/spike_detail_window.h"

#include <cmath>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui/widgets/spike_grid_view.h"

namespace ccv2 {

SpikeDetailWindow::SpikeDetailWindow(QWidget *parent)
    : QDialog(parent, Qt::Window | Qt::WindowCloseButtonHint |
                          Qt::WindowMinMaxButtonsHint) {
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setWindowTitle(QStringLiteral("单通道 Spike"));
    resize(920, 620);
    setMinimumSize(680, 460);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *header = new QHBoxLayout;
    m_title = new QLabel(QStringLiteral("未选择通道"));
    m_title->setObjectName(QStringLiteral("title"));
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(m_title, 1);
    m_stats = new QLabel;
    m_stats->setProperty("role", "data-value");
    header->addWidget(m_stats);
    root->addLayout(header);

    auto *controls = new QWidget;
    controls->setProperty("role", "page-control-panel");
    auto *controlLayout = new QVBoxLayout(controls);
    controlLayout->setContentsMargins(8, 6, 8, 6);
    controlLayout->setSpacing(6);

    auto *thresholdRow = new QHBoxLayout;
    thresholdRow->setContentsMargins(0, 0, 0, 0);
    thresholdRow->setSpacing(8);

    m_overrideCheck = new QCheckBox(QStringLiteral("独立阈值"));
    m_overrideCheck->setToolTip(
        QStringLiteral("启用后，该通道不再跟随页面顶部的全局阈值"));
    thresholdRow->addWidget(m_overrideCheck);

    auto *thresholdLabel = new QLabel(QStringLiteral("输入阈值"));
    thresholdLabel->setProperty("role", "field-label");
    thresholdRow->addWidget(thresholdLabel);
    m_thresholdSpin = new QDoubleSpinBox;
    m_thresholdSpin->setRange(1.0, 5000.0);
    m_thresholdSpin->setDecimals(1);
    m_thresholdSpin->setSingleStep(1.0);
    m_thresholdSpin->setSuffix(QStringLiteral(" µV"));
    thresholdRow->addWidget(m_thresholdSpin);

    m_hint = new QLabel(
        QStringLiteral("跟随全局；启用独立阈值后可拖动图中的虚线"));
    m_hint->setProperty("role", "caption");
    m_hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    thresholdRow->addWidget(m_hint, 1);
    controlLayout->addLayout(thresholdRow);

    auto *displayRow = new QHBoxLayout;
    displayRow->setContentsMargins(0, 0, 0, 0);
    displayRow->setSpacing(8);
    auto *scaleLabel = new QLabel(QStringLiteral("纵向量程"));
    scaleLabel->setProperty("role", "field-label");
    displayRow->addWidget(scaleLabel);
    m_yScaleSpin = new QDoubleSpinBox;
    m_yScaleSpin->setRange(10.0, 5000.0);
    m_yScaleSpin->setDecimals(0);
    m_yScaleSpin->setSingleStep(25.0);
    m_yScaleSpin->setValue(200.0);
    m_yScaleSpin->setSuffix(QStringLiteral(" µV"));
    displayRow->addWidget(m_yScaleSpin);
    m_autoScaleCheck = new QCheckBox(QStringLiteral("自动量程"));
    displayRow->addWidget(m_autoScaleCheck);

    auto *overlayLabel = new QLabel(QStringLiteral("叠加"));
    overlayLabel->setProperty("role", "field-label");
    displayRow->addWidget(overlayLabel);
    m_overlaySpin = new QSpinBox;
    m_overlaySpin->setRange(10, 2000);
    m_overlaySpin->setSingleStep(10);
    m_overlaySpin->setValue(100);
    m_overlaySpin->setSuffix(QStringLiteral(" 条"));
    displayRow->addWidget(m_overlaySpin);
    m_fadeCheck = new QCheckBox(QStringLiteral("旧波形淡出"));
    m_fadeCheck->setChecked(true);
    displayRow->addWidget(m_fadeCheck);
    auto *displayHint = new QLabel(QStringLiteral("仅影响此单通道窗口"));
    displayHint->setProperty("role", "caption");
    displayRow->addWidget(displayHint);
    displayRow->addStretch(1);
    controlLayout->addLayout(displayRow);
    root->addWidget(controls);

    auto *plotWrap = new QWidget;
    plotWrap->setProperty("role", "plot-area");
    auto *plotLayout = new QVBoxLayout(plotWrap);
    plotLayout->setContentsMargins(6, 6, 6, 6);
    m_view = new SpikeGridView;
    m_view->setSnippetDrawLimits(32, 100);
    m_view->setPlaceholderText(QStringLiteral("等待该通道的 Spike 留存波形"));
    m_view->setFocusLane(-1);
    plotLayout->addWidget(m_view);
    root->addWidget(plotWrap, 1);

    connect(m_overrideCheck, &QCheckBox::toggled, this, [this](bool) {
        if (m_updating) return;
        updateThresholdEditingState();
        emitThresholdOverride();
    });
    connect(m_thresholdSpin, &QDoubleSpinBox::valueChanged, this, [this](double) {
        if (!m_updating && m_overrideCheck->isChecked()) {
            emitThresholdOverride();
        }
    });
    connect(m_view, &SpikeGridView::thresholdDragged, this,
            [this](double thresholdV) {
        if (!m_overrideCheck->isChecked()) return;
        const QSignalBlocker blocker(m_thresholdSpin);
        m_thresholdSpin->setValue(std::abs(thresholdV) * 1e6);
        emitThresholdOverride();
    });
    auto applyDisplayOptions = [this]() {
        const int overlays = m_overlaySpin->value();
        m_view->setYFullScaleMicrovolts(m_yScaleSpin->value());
        m_view->setAutoScale(m_autoScaleCheck->isChecked());
        m_view->setFadeEnabled(m_fadeCheck->isChecked());
        m_view->setSnippetDrawLimits(qMin(32, overlays), overlays);
        emit displayOptionsChanged(
            m_yScaleSpin->value(), m_autoScaleCheck->isChecked(),
            m_fadeCheck->isChecked(), overlays);
    };
    connect(m_yScaleSpin, &QDoubleSpinBox::valueChanged, this,
            [applyDisplayOptions](double) { applyDisplayOptions(); });
    connect(m_autoScaleCheck, &QCheckBox::toggled, this,
            [applyDisplayOptions](bool) { applyDisplayOptions(); });
    connect(m_overlaySpin, &QSpinBox::valueChanged, this,
            [applyDisplayOptions](int) { applyDisplayOptions(); });
    connect(m_fadeCheck, &QCheckBox::toggled, this,
            [applyDisplayOptions](bool) { applyDisplayOptions(); });

    updateThresholdEditingState();
}

void SpikeDetailWindow::setStore(SpikeSnippetStore *store) {
    m_view->setStore(store);
}

void SpikeDetailWindow::setLane(int lane, const QString &name,
                                bool overrideEnabled,
                                double overrideMagnitudeUv,
                                bool negativePolarity) {
    m_updating = true;
    m_lane = lane;
    m_negativePolarity = negativePolarity;
    m_title->setText(name);
    setWindowTitle(QStringLiteral("单通道 Spike · %1").arg(name));
    m_overrideCheck->setChecked(overrideEnabled);
    if (overrideMagnitudeUv > 0.0) {
        m_thresholdSpin->setValue(overrideMagnitudeUv);
    }
    m_view->setFocusLane(lane);
    m_view->setSelectedLane(-1);
    if (overrideEnabled) {
        if (m_thresholds.size() <= lane) m_thresholds.resize(lane + 1);
        m_thresholds[lane] =
            (negativePolarity ? -overrideMagnitudeUv : overrideMagnitudeUv) *
            1e-6;
        m_view->setThresholds(m_thresholds);
    }
    m_updating = false;
    updateThresholdEditingState();
}

void SpikeDetailWindow::setTimebase(double sampleRate, int preSamples) {
    m_view->setTimebase(sampleRate, preSamples);
}

void SpikeDetailWindow::setDisplayOptions(double yFullScaleUv, bool autoScale,
                                          bool fade, int overlayCount) {
    const QSignalBlocker scaleBlocker(m_yScaleSpin);
    const QSignalBlocker autoBlocker(m_autoScaleCheck);
    const QSignalBlocker fadeBlocker(m_fadeCheck);
    const QSignalBlocker overlayBlocker(m_overlaySpin);
    m_yScaleSpin->setValue(yFullScaleUv);
    m_autoScaleCheck->setChecked(autoScale);
    m_fadeCheck->setChecked(fade);
    m_overlaySpin->setValue(overlayCount);
    m_view->setYFullScaleMicrovolts(yFullScaleUv);
    m_view->setAutoScale(autoScale);
    m_view->setFadeEnabled(fade);
    m_view->setSnippetDrawLimits(qMin(32, overlayCount), overlayCount);
}

void SpikeDetailWindow::setWaveTheme(
    const QMap<QString, QString> &palette) {
    m_view->setThemePalette(palette);
}

void SpikeDetailWindow::updateData(const QVector<qint64> &totals,
                                   const QVector<double> &thresholdsV,
                                   const QVector<double> &rmsV,
                                   const QVector<double> &ratesHz) {
    if (m_lane < 0) return;
    m_thresholds = thresholdsV;
    // The worker publishes stats every 200 ms. During a drag, keep the
    // editor's value authoritative so the 20 Hz GUI refresh cannot snap the
    // threshold line back to the previous published value.
    if (m_overrideCheck->isChecked()) {
        if (m_thresholds.size() <= m_lane) m_thresholds.resize(m_lane + 1);
        m_thresholds[m_lane] =
            (m_negativePolarity ? -m_thresholdSpin->value()
                                : m_thresholdSpin->value()) *
            1e-6;
    }
    m_view->setThresholds(m_thresholds);
    m_view->setRates(ratesHz);

    const double threshold = m_thresholds.value(m_lane, 0.0);
    const double rms = rmsV.value(m_lane, 0.0);
    const qint64 spikes = totals.value(m_lane, 0);
    const double rate = ratesHz.value(m_lane, 0.0);
    m_stats->setText(
        QStringLiteral("输入等效阈值 %1 µV   噪声 %2 µVrms   %3 spike   %4 Hz")
            .arg(threshold * 1e6, 0, 'f', 1)
            .arg(rms * 1e6, 0, 'f', 1)
            .arg(spikes)
            .arg(rate, 0, 'f', 1));

    if (!m_overrideCheck->isChecked() && std::abs(threshold) > 0.0) {
        const QSignalBlocker blocker(m_thresholdSpin);
        m_thresholdSpin->setValue(std::abs(threshold) * 1e6);
    }
}

void SpikeDetailWindow::pullNewSnippets() {
    if (isVisible()) m_view->pullNewSnippets();
}

void SpikeDetailWindow::clearTraces() {
    m_view->clearTraces();
}

void SpikeDetailWindow::emitThresholdOverride() {
    if (m_lane < 0) return;
    if (m_overrideCheck->isChecked()) {
        if (m_thresholds.size() <= m_lane) m_thresholds.resize(m_lane + 1);
        m_thresholds[m_lane] =
            (m_negativePolarity ? -m_thresholdSpin->value()
                                : m_thresholdSpin->value()) *
            1e-6;
        m_view->setThresholds(m_thresholds);
    }
    emit thresholdOverrideChanged(m_lane, m_overrideCheck->isChecked(),
                                  m_thresholdSpin->value());
}

void SpikeDetailWindow::updateThresholdEditingState() {
    const bool enabled = m_overrideCheck->isChecked();
    m_thresholdSpin->setEnabled(enabled);
    m_view->setThresholdEditingEnabled(enabled);
    m_hint->setText(
        enabled
            ? QStringLiteral("可输入数值，或在图中上下拖动阈值虚线")
            : QStringLiteral("当前跟随页面顶部的全局阈值"));
}

}  // namespace ccv2
