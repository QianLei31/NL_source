#include "ui/widgets/recording_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStorageInfo>
#include <QStyle>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QtConcurrent>

#include "io/matlab_channel_exporter.h"
#include "io/session_manifest.h"

namespace ccv2 {

namespace {

QLabel *fieldLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setProperty("role", "field-label");
    return label;
}

}  // namespace

RecordingPanel::RecordingPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("spiControlPanel"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *group = new QGroupBox(QStringLiteral("原始流录制"));
    auto *form = new QVBoxLayout(group);
    form->setContentsMargins(8, 12, 8, 8);
    form->setSpacing(6);

    form->addWidget(fieldLabel(QStringLiteral("保存目录")));
    auto *directoryRow = new QHBoxLayout;
    directoryRow->setSpacing(6);
    m_saveDirEdit = new QLineEdit(QStringLiteral("d:/ADC_data"));
    m_browseDirBtn = new QPushButton(QStringLiteral("浏览"));
    m_browseDirBtn->setProperty("variant", "secondary");
    directoryRow->addWidget(m_saveDirEdit, 1);
    directoryRow->addWidget(m_browseDirBtn);
    form->addLayout(directoryRow);

    form->addWidget(fieldLabel(QStringLiteral("会话名")));
    m_sessionNameEdit = new QLineEdit;
    m_sessionNameEdit->setPlaceholderText(QStringLiteral("留空=按时间自动命名"));
    form->addWidget(m_sessionNameEdit);

    auto *splitRow = new QHBoxLayout;
    splitRow->setSpacing(6);
    splitRow->addWidget(fieldLabel(QStringLiteral("分片大小")));
    m_splitCombo = new QComboBox;
    m_splitCombo->addItem(QStringLiteral("不分片"), QVariant::fromValue<qint64>(0));
    m_splitCombo->addItem(QStringLiteral("1 GB"), QVariant::fromValue<qint64>(1LL << 30));
    m_splitCombo->addItem(QStringLiteral("2 GB"), QVariant::fromValue<qint64>(2LL << 30));
    m_splitCombo->addItem(QStringLiteral("5 GB"), QVariant::fromValue<qint64>(5LL << 30));
    m_splitCombo->setCurrentIndex(2);
    splitRow->addWidget(m_splitCombo, 1);
    form->addLayout(splitRow);

    m_freeSpaceLabel = new QLabel(QStringLiteral("剩余空间: --"));
    m_freeSpaceLabel->setProperty("role", "caption");
    form->addWidget(m_freeSpaceLabel);

    m_recordStatusLabel = new QLabel(QStringLiteral("录制在顶部工具栏控制"));
    m_recordStatusLabel->setProperty("role", "caption");
    m_recordStatusLabel->setWordWrap(true);
    form->addWidget(m_recordStatusLabel);

    root->addWidget(group);

    // === Threshold-triggered recording ===
    auto *trigGroup = new QGroupBox(QStringLiteral("触发录制"));
    auto *tform = new QVBoxLayout(trigGroup);
    tform->setContentsMargins(8, 12, 8, 8);
    tform->setSpacing(6);

    m_trigEnable = new QCheckBox(QStringLiteral("按阈值自动起录"));
    m_trigEnable->setToolTip(QStringLiteral(
        "启用后，接收中若指定通道越过阈值即自动开始录制。"));
    tform->addWidget(m_trigEnable);

    auto *chRow = new QHBoxLayout;
    chRow->setSpacing(6);
    chRow->addWidget(fieldLabel(QStringLiteral("触发通道")));
    m_trigChannel = new QSpinBox;
    m_trigChannel->setRange(0, 255);
    chRow->addWidget(m_trigChannel, 1);
    tform->addLayout(chRow);

    auto *thRow = new QHBoxLayout;
    thRow->setSpacing(6);
    thRow->addWidget(fieldLabel(QStringLiteral("阈值")));
    m_trigThreshold = new QDoubleSpinBox;
    m_trigThreshold->setRange(0.0, 1800.0);
    m_trigThreshold->setDecimals(0);
    m_trigThreshold->setSingleStep(10.0);
    m_trigThreshold->setValue(900.0);
    m_trigThreshold->setSuffix(QStringLiteral(" mV"));
    thRow->addWidget(m_trigThreshold, 1);
    tform->addLayout(thRow);

    auto *polRow = new QHBoxLayout;
    polRow->setSpacing(6);
    polRow->addWidget(fieldLabel(QStringLiteral("极性")));
    m_trigPolarity = new QComboBox;
    m_trigPolarity->addItem(QStringLiteral("上升沿越过"), true);
    m_trigPolarity->addItem(QStringLiteral("下降沿跌破"), false);
    polRow->addWidget(m_trigPolarity, 1);
    tform->addLayout(polRow);

    // TDM electrode selector — only meaningful (and only shown) when the global
    // TDM mode is on. Hidden by default; setTdmMode() reveals it and labels
    // the two currently visible phases.
    m_trigTdmRow = new QWidget;
    auto *tdmRow = new QHBoxLayout(m_trigTdmRow);
    tdmRow->setContentsMargins(0, 0, 0, 0);
    tdmRow->setSpacing(6);
    tdmRow->addWidget(fieldLabel(QStringLiteral("触发电极")));
    m_trigTdmSlot = new QComboBox;
    m_trigTdmSlot->addItem(QStringLiteral("相位 0"), 0);
    m_trigTdmSlot->addItem(QStringLiteral("相位 2"), 1);
    m_trigTdmSlot->setToolTip(QStringLiteral(
        "TDM 模式下选择当前显示对中的一个物理相位判定阈值越过。"));
    tdmRow->addWidget(m_trigTdmSlot, 1);
    tform->addWidget(m_trigTdmRow);
    m_trigTdmRow->setVisible(false);

    auto *stopRow = new QHBoxLayout;
    stopRow->setSpacing(6);
    stopRow->addWidget(fieldLabel(QStringLiteral("自动停止")));
    m_trigAutoStop = new QDoubleSpinBox;
    m_trigAutoStop->setRange(0.0, 3600.0);
    m_trigAutoStop->setDecimals(0);
    m_trigAutoStop->setSingleStep(5.0);
    m_trigAutoStop->setValue(0.0);
    m_trigAutoStop->setSuffix(QStringLiteral(" 秒"));
    m_trigAutoStop->setSpecialValueText(QStringLiteral("手动停止"));
    m_trigAutoStop->setToolTip(QStringLiteral(
        "0=手动停止；>0=录制该秒数后自动停止并重新武装，可重复捕获。"));
    stopRow->addWidget(m_trigAutoStop, 1);
    tform->addLayout(stopRow);

    root->addWidget(trigGroup);

    // === Offline MATLAB export ===
    auto *exportGroup = new QGroupBox(QStringLiteral("离线导出"));
    auto *xform = new QVBoxLayout(exportGroup);
    xform->setContentsMargins(8, 12, 8, 8);
    xform->setSpacing(6);

    auto *loadRow = new QHBoxLayout;
    loadRow->setSpacing(6);
    m_loadBinBtn = new QPushButton(QStringLiteral("加载 BIN"));
    m_loadBinBtn->setProperty("variant", "secondary");
    m_exportBtn = new QPushButton(QStringLiteral("导出 MATLAB"));
    m_exportBtn->setProperty("variant", "primary");
    m_exportBtn->setEnabled(false);
    loadRow->addWidget(m_loadBinBtn);
    loadRow->addWidget(m_exportBtn);
    xform->addLayout(loadRow);

    m_exportFileLabel = new QLabel(QStringLiteral("未加载"));
    m_exportFileLabel->setProperty("role", "caption");
    m_exportFileLabel->setWordWrap(true);
    xform->addWidget(m_exportFileLabel);

    auto *fsRow = new QHBoxLayout;
    fsRow->setSpacing(6);
    fsRow->addWidget(fieldLabel(QStringLiteral("采样率")));
    m_exportFsSpin = new QDoubleSpinBox;
    m_exportFsSpin->setRange(1.0, 1e6);
    m_exportFsSpin->setDecimals(0);
    m_exportFsSpin->setValue(20000.0);
    m_exportFsSpin->setSuffix(QStringLiteral(" Hz"));
    fsRow->addWidget(m_exportFsSpin, 1);
    xform->addLayout(fsRow);

    m_exportTdmCheck = new QCheckBox(QStringLiteral("TDM 电极分拣导出"));
    xform->addWidget(m_exportTdmCheck);
    m_exportRaw32Check = new QCheckBox(QStringLiteral("保留原始32位（含时间戳）"));
    m_exportRaw32Check->setToolTip(
        QStringLiteral("关闭时默认仅导出低12位ADC值；TDM导出始终只保留ADC值"));
    xform->addWidget(m_exportRaw32Check);
    m_exportTdmPhase = new QComboBox;
    m_exportTdmPhase->addItem(QStringLiteral("四相 0 / 1 / 2 / 3 全部导出"), 0);
    m_exportTdmPhase->setEnabled(false);
    xform->addWidget(m_exportTdmPhase);

    m_exportStatusLabel = new QLabel;
    m_exportStatusLabel->setProperty("role", "status-line");
    m_exportStatusLabel->setProperty("state", "info");
    m_exportStatusLabel->setWordWrap(true);
    m_exportStatusLabel->setVisible(false);
    xform->addWidget(m_exportStatusLabel);

    root->addWidget(exportGroup);
    root->addStretch(1);

    connect(m_exportTdmCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_exportTdmPhase) m_exportTdmPhase->setEnabled(false);
        if (m_exportRaw32Check) {
            if (checked) m_exportRaw32Check->setChecked(false);
            m_exportRaw32Check->setEnabled(!checked);
        }
    });
    connect(m_loadBinBtn, &QPushButton::clicked, this, &RecordingPanel::loadBinForExport);
    connect(m_exportBtn, &QPushButton::clicked, this, &RecordingPanel::exportLoadedBinToMatlab);

    auto emitTrigger = [this]() { emit triggerSettingsChanged(); };
    connect(m_trigEnable, &QCheckBox::toggled, this, emitTrigger);
    connect(m_trigChannel, qOverload<int>(&QSpinBox::valueChanged), this,
            [emitTrigger](int) { emitTrigger(); });
    connect(m_trigThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitTrigger](double) { emitTrigger(); });
    connect(m_trigAutoStop, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitTrigger](double) { emitTrigger(); });
    connect(m_trigPolarity, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [emitTrigger](int) { emitTrigger(); });
    connect(m_trigTdmSlot, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [emitTrigger](int) { emitTrigger(); });

    connect(m_browseDirBtn, &QPushButton::clicked, this, [this]() {
        const QString directory = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("选择保存目录"),
            m_saveDirEdit->text());
        if (!directory.isEmpty()) {
            m_saveDirEdit->setText(directory);
            refreshFreeSpace();
            emit settingsChanged();
        }
    });
    connect(m_saveDirEdit, &QLineEdit::editingFinished, this, [this]() {
        refreshFreeSpace();
        emit settingsChanged();
    });
    connect(m_sessionNameEdit, &QLineEdit::editingFinished,
            this, &RecordingPanel::settingsChanged);
    connect(m_splitCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { emit settingsChanged(); });
    refreshFreeSpace();
}

QString RecordingPanel::saveDir() const
{
    return m_saveDirEdit ? m_saveDirEdit->text().trimmed() : QString();
}

QString RecordingPanel::sessionName() const
{
    return m_sessionNameEdit
               ? m_sessionNameEdit->text().trimmed()
               : QString();
}

qint64 RecordingPanel::splitBytes() const
{
    return m_splitCombo ? m_splitCombo->currentData().toLongLong() : 0;
}

void RecordingPanel::setSaveDir(const QString &dir)
{
    if (!m_saveDirEdit || dir.isEmpty()) return;
    const QSignalBlocker blocker(m_saveDirEdit);
    m_saveDirEdit->setText(dir);
    refreshFreeSpace();
}

void RecordingPanel::setSessionName(const QString &name)
{
    if (!m_sessionNameEdit) return;
    const QSignalBlocker blocker(m_sessionNameEdit);
    m_sessionNameEdit->setText(name);
}

void RecordingPanel::setSplitBytes(qint64 bytes)
{
    if (!m_splitCombo) return;
    const int index = m_splitCombo->findData(QVariant::fromValue(bytes));
    const QSignalBlocker blocker(m_splitCombo);
    m_splitCombo->setCurrentIndex(index >= 0 ? index : 2);
}

void RecordingPanel::setRecordingStatus(bool recording, const QString &text)
{
    if (m_recordStatusLabel) {
        m_recordStatusLabel->setText(
            text.isEmpty()
                ? (recording ? QStringLiteral("录制中")
                             : QStringLiteral("未录制"))
                : text);
        m_recordStatusLabel->setProperty("state", recording ? "ok" : "info");
        m_recordStatusLabel->style()->unpolish(m_recordStatusLabel);
        m_recordStatusLabel->style()->polish(m_recordStatusLabel);
    }
    if (m_saveDirEdit) m_saveDirEdit->setEnabled(!recording);
    if (m_browseDirBtn) m_browseDirBtn->setEnabled(!recording);
    if (m_sessionNameEdit) m_sessionNameEdit->setEnabled(!recording);
    if (m_splitCombo) m_splitCombo->setEnabled(!recording);
}

RecordingPanel::~RecordingPanel()
{
    m_shuttingDown.store(true);
    m_exportCancel.store(true);
    if (m_exportFuture.isRunning()) {
        m_exportFuture.waitForFinished();
    }
}

namespace {
void setExportStatus(QLabel *label, const QString &text, const char *state) {
    if (!label) return;
    label->setVisible(!text.isEmpty());
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}
}  // namespace

void RecordingPanel::loadBinForExport()
{
    const QString startPath = m_loadedBin.isEmpty()
                                  ? saveDir()
                                  : QFileInfo(m_loadedBin).absolutePath();
    const QString adc = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要导出的 .bin 文件"), startPath,
        QStringLiteral("Binary files (*.bin);;All files (*.*)"));
    if (adc.isEmpty()) return;

    const QFileInfo info(adc);
    if (!info.exists() || !info.isFile()) {
        setExportStatus(m_exportStatusLabel, QStringLiteral("所选文件不存在"), "error");
        return;
    }
    m_loadedBin = adc;
    if (m_exportFileLabel) {
        m_exportFileLabel->setText(info.fileName());
        m_exportFileLabel->setToolTip(adc);
    }
    if (m_exportBtn) m_exportBtn->setEnabled(true);

    // Auto-fill the sample rate (and, when recorded, the TDM mode/phase) from
    // the recording's manifest if present.
    SessionInput input;
    QString err;
    if (!SessionManifest::resolveInput(adc, m_exportFsSpin->value(), &input, &err)) {
        m_loadedBin.clear();
        m_exportBtn->setEnabled(false);
        setExportStatus(m_exportStatusLabel, err, "error");
        return;
    }
    if (input.hasManifest && input.metadata.sampleRate > 0.0 && m_exportFsSpin) {
        {
            const QSignalBlocker b(m_exportFsSpin);
            m_exportFsSpin->setValue(input.metadata.sampleRate);
        }
        QString status = QStringLiteral("已加载，采样率取自 session.json");
        if (input.metadata.tdmKnown) {
            // No signal blockers: the checkbox handler keeps the phase combo
            // and RAW32 checkbox consistent. Manual override stays possible.
            if (m_exportTdmCheck) {
                m_exportTdmCheck->setChecked(input.metadata.tdmEnabled);
            }
            if (m_exportTdmPhase) {
                // Export always writes all four phases. The live display's
                // 0/2 versus 1/3 choice is not an export mode, and index 1
                // does not exist in this informational one-item combo.
                m_exportTdmPhase->setCurrentIndex(0);
            }
            status = input.metadata.tdmEnabled
                         ? QStringLiteral("已加载，采样率与TDM模式取自 session.json（四相全部导出）")
                         : QStringLiteral("已加载，采样率取自 session.json（录制时TDM关闭）");
        }
        setExportStatus(m_exportStatusLabel, status + input.warning, input.warning.isEmpty() ? "ok" : "warn");
    } else {
        setExportStatus(m_exportStatusLabel, QString(), "info");
    }
}

void RecordingPanel::exportLoadedBinToMatlab()
{
    if (m_shuttingDown.load() || m_loadedBin.isEmpty()) return;
    if (m_exportBusy.load()) {
        setExportStatus(m_exportStatusLabel, QStringLiteral("导出正在进行"), "warn");
        return;
    }
    const QFileInfo adcInfo(m_loadedBin);
    const bool tdm = m_exportTdmCheck && m_exportTdmCheck->isChecked();
    const bool raw32 = !tdm && m_exportRaw32Check && m_exportRaw32Check->isChecked();
    const bool evenFirst = !m_exportTdmPhase || m_exportTdmPhase->currentData().toInt() == 0;
    const QString defaultDir = adcInfo.dir().filePath(
        adcInfo.completeBaseName() +
        (tdm ? QStringLiteral("_matlab_tdm_ele")
             : (raw32 ? QStringLiteral("_matlab_raw32")
                      : QStringLiteral("_matlab_adc12"))));
    const QString outputDir =
        QFileDialog::getExistingDirectory(this, QStringLiteral("选择 MATLAB 导出目录"), defaultDir);
    if (outputDir.isEmpty()) return;

    m_exportBusy.store(true);
    m_exportCancel.store(false);
    if (m_exportBtn) m_exportBtn->setEnabled(false);
    setExportStatus(m_exportStatusLabel,
                    tdm ? QStringLiteral("正在导出 TDM 分拣电极…")
                        : QStringLiteral("正在导出 256 通道…"),
                    "info");

    const QString adcFile = m_loadedBin;
    const double fs = m_exportFsSpin ? m_exportFsSpin->value() : 20000.0;
    MatlabChannelExportOptions options;
    options.mode = tdm ? MatlabExportMode::TdmDemux
                       : (raw32 ? MatlabExportMode::RawWords
                                : MatlabExportMode::RawAdc);
    options.evenSampleIsFirstLocalEle = evenFirst;
    options.cancelFlag = &m_exportCancel;
    QPointer<RecordingPanel> self(this);
    m_exportFuture = QtConcurrent::run([self, adcFile, outputDir, fs, options]() {
        const MatlabChannelExportResult result =
            MatlabChannelExporter::exportAdcBin(adcFile, outputDir, fs, options);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, result]() {
            if (!self || self->m_shuttingDown.load()) {
                if (self) self->m_exportBusy.store(false);
                return;
            }
            self->m_exportBusy.store(false);
            if (self->m_exportBtn) self->m_exportBtn->setEnabled(true);
            if (!result.ok) {
                setExportStatus(self->m_exportStatusLabel,
                                QStringLiteral("导出失败: %1").arg(result.error), "error");
                return;
            }
            setExportStatus(self->m_exportStatusLabel,
                             QStringLiteral("导出完成 %1 帧 / %2 输出%3")
                                 .arg(result.frameCount).arg(result.outputCount)
                                 .arg(result.warning.isEmpty() ? QString() : QStringLiteral("\n") + result.warning),
                             result.warning.isEmpty() ? "ok" : "warn");
        }, Qt::QueuedConnection);
    });
}

bool RecordingPanel::triggerEnabled() const {
    return m_trigEnable && m_trigEnable->isChecked();
}

int RecordingPanel::triggerChannel() const {
    return m_trigChannel ? m_trigChannel->value() : 0;
}

double RecordingPanel::triggerThresholdVolts() const {
    return m_trigThreshold ? m_trigThreshold->value() / 1000.0 : 0.9;
}

bool RecordingPanel::triggerRisingAbove() const {
    return m_trigPolarity ? m_trigPolarity->currentData().toBool() : true;
}

double RecordingPanel::triggerAutoStopSeconds() const {
    return m_trigAutoStop ? m_trigAutoStop->value() : 0.0;
}

int RecordingPanel::triggerTdmSlot() const {
    // Explicit row visibility represents TDM mode; ancestor visibility does
    // not. The storage pane is normally hidden behind hardware controls, but
    // that must never silently broaden a selected-electrode trigger to all
    // source frames (including while restoring settings before first show).
    if (!m_trigTdmRow || m_trigTdmRow->isHidden() || !m_trigTdmSlot) {
        return -1;
    }
    return m_trigTdmSlot->currentData().toInt();
}

void RecordingPanel::setTriggerTdmSlot(int slot) {
    if (!m_trigTdmSlot) return;
    const QSignalBlocker b(m_trigTdmSlot);
    const int idx = m_trigTdmSlot->findData(slot);
    m_trigTdmSlot->setCurrentIndex(idx >= 0 ? idx : 0);
}

void RecordingPanel::setTdmMode(bool enabled, bool pair02) {
    if (m_trigTdmRow) m_trigTdmRow->setVisible(enabled);
    if (m_trigTdmSlot) {
        const QSignalBlocker blocker(m_trigTdmSlot);
        const int selected = m_trigTdmSlot->currentIndex();
        m_trigTdmSlot->setItemText(0, pair02 ? QStringLiteral("相位 0")
                                             : QStringLiteral("相位 1"));
        m_trigTdmSlot->setItemText(1, pair02 ? QStringLiteral("相位 2")
                                             : QStringLiteral("相位 3"));
        m_trigTdmSlot->setCurrentIndex(qBound(0, selected, 1));
    }
}

void RecordingPanel::setTriggerSettings(bool enabled, int channel, double thresholdMv,
                                        bool risingAbove, double autoStopSeconds) {
    if (m_trigEnable) { const QSignalBlocker b(m_trigEnable); m_trigEnable->setChecked(enabled); }
    if (m_trigChannel) { const QSignalBlocker b(m_trigChannel); m_trigChannel->setValue(channel); }
    if (m_trigThreshold) { const QSignalBlocker b(m_trigThreshold); m_trigThreshold->setValue(thresholdMv); }
    if (m_trigPolarity) {
        const QSignalBlocker b(m_trigPolarity);
        m_trigPolarity->setCurrentIndex(risingAbove ? 0 : 1);
    }
    if (m_trigAutoStop) { const QSignalBlocker b(m_trigAutoStop); m_trigAutoStop->setValue(autoStopSeconds); }
}

void RecordingPanel::refreshFreeSpace()
{
    if (!m_freeSpaceLabel || !m_saveDirEdit) return;
    QStorageInfo info(m_saveDirEdit->text());
    if (!info.isValid() || !info.isReady()) {
        info = QStorageInfo(QDir(m_saveDirEdit->text()).absolutePath());
    }
    if (info.isValid() && info.isReady()) {
        const double gb =
            info.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
        m_freeSpaceLabel->setText(
            QStringLiteral("剩余空间: %1 GB").arg(gb, 0, 'f', 1));
    } else {
        m_freeSpaceLabel->setText(QStringLiteral("剩余空间: --"));
    }
}

}  // namespace ccv2
