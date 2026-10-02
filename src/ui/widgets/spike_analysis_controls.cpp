#include "ui/widgets/spike_analysis_controls.h"

#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace ccv2 {
namespace {
QPushButton *button(const QString &text, const char *name, QWidget *parent = nullptr) {
    auto *b = new QPushButton(text, parent); b->setObjectName(QLatin1String(name));
    b->setProperty("variant", "secondary"); return b;
}
QString lifecycle(SpikeArchiveState state) {
    switch (state) {
    case SpikeArchiveState::Idle: return QStringLiteral("未启动");
    case SpikeArchiveState::Running: return QStringLiteral("正在归档");
    case SpikeArchiveState::Completed: return QStringLiteral("档案写入完成");
    case SpikeArchiveState::Drained: return QStringLiteral("已停止并排空归档队列");
    case SpikeArchiveState::Cancelled: return QStringLiteral("已取消；保留已接受事件");
    case SpikeArchiveState::Failed: return QStringLiteral("归档失败；检查可恢复前缀");
    }
    return {};
}
}

SpikeAnalysisControls::SpikeAnalysisControls(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("spikeAnalysisControls"));
    setProperty("role", "page-control-panel");
    auto *root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(6); root->setAlignment(Qt::AlignTop);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto *top = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("事件归档与规则")); title->setObjectName(QStringLiteral("title")); top->addWidget(title);
    top->addStretch();
    auto *rule = button(QStringLiteral("波形框规则…"), "analysisRuleEditor"); top->addWidget(rule);
    auto *open = button(QStringLiteral("打开归档…"), "analysisOpenArchive"); top->addWidget(open);
    root->addLayout(top);
    auto *row = new QGridLayout; row->setHorizontalSpacing(6); row->setVerticalSpacing(6);
    auto *destinationLabel = new QLabel(QStringLiteral("归档目录"));
    m_archivePath = new QLineEdit; m_archivePath->setObjectName(QStringLiteral("analysisArchiveDestination"));
    m_archivePath->setPlaceholderText(QStringLiteral("新的 sidecar 目录；不修改原始录制"));
    destinationLabel->setBuddy(m_archivePath); row->addWidget(destinationLabel, 0, 0); row->addWidget(m_archivePath, 0, 1);
    auto *choose = m_chooseDestination = button(QStringLiteral("选择…"), "analysisChooseDestination"); row->addWidget(choose, 0, 2);
    m_start = button(QStringLiteral("开始归档"), "analysisStartArchive"); m_start->setProperty("variant", "primary"); row->addWidget(m_start, 0, 3);
    m_stop = button(QStringLiteral("停止并排空"), "analysisStopArchive"); row->addWidget(m_stop, 0, 4);
    row->setColumnStretch(1, 1); root->addLayout(row);
    m_archiveStatus = new QLabel(QStringLiteral("归档未启动 · 留存波形窗口不是完整事件记录"));
    m_archiveStatus->setObjectName(QStringLiteral("analysisArchiveStatus")); m_archiveStatus->setWordWrap(true); m_archiveStatus->setProperty("role", "caption"); root->addWidget(m_archiveStatus);
    auto *offlineToggle = button(QStringLiteral("整段录制分析 ▾"), "analysisOfflineToggle"); offlineToggle->setCheckable(true); root->addWidget(offlineToggle, 0, Qt::AlignLeft);
    auto *offline = new QWidget; offline->setObjectName(QStringLiteral("analysisOfflinePanel")); offline->setProperty("role", "page-control-panel");
    auto *offlineLayout = new QGridLayout(offline); offlineLayout->setContentsMargins(0, 0, 0, 0); offlineLayout->setSpacing(6);
    m_recordingPath = new QLineEdit; m_recordingPath->setObjectName(QStringLiteral("analysisRecordingDirectory"));
    m_recordingPath->setPlaceholderText(QStringLiteral("包含 session.json 的录制目录"));
    m_outputPath = new QLineEdit; m_outputPath->setObjectName(QStringLiteral("analysisOutputDirectory"));
    m_outputPath->setPlaceholderText(QStringLiteral("新的独立归档输出目录"));
    auto *sourceLabel = new QLabel(QStringLiteral("源录制")); sourceLabel->setBuddy(m_recordingPath);
    auto *outputLabel = new QLabel(QStringLiteral("输出")); outputLabel->setBuddy(m_outputPath);
    auto *sourceChoose = m_chooseRecording = button(QStringLiteral("选择…"), "analysisChooseRecording");
    auto *outputChoose = m_chooseOutput = button(QStringLiteral("选择…"), "analysisChooseOutput");
    offlineLayout->addWidget(sourceLabel, 0, 0); offlineLayout->addWidget(m_recordingPath, 0, 1); offlineLayout->addWidget(sourceChoose, 0, 2);
    offlineLayout->addWidget(outputLabel, 1, 0); offlineLayout->addWidget(m_outputPath, 1, 1); offlineLayout->addWidget(outputChoose, 1, 2);
    m_analyze = button(QStringLiteral("分析整段录制"), "analysisRunOffline"); m_analyze->setProperty("variant", "primary");
    m_cancel = button(QStringLiteral("取消分析"), "analysisCancelOffline");
    offlineLayout->addWidget(m_analyze, 0, 3); offlineLayout->addWidget(m_cancel, 1, 3);
    m_progress = new QProgressBar; m_progress->setObjectName(QStringLiteral("analysisOfflineProgress")); m_progress->setRange(0, 1000); m_progress->setValue(0);
    offlineLayout->addWidget(m_progress, 2, 0, 1, 4);
    m_offlineStatus = new QLabel(QStringLiteral("使用独立分析任务；不会移动当前回放位置或启动硬件采集"));
    m_offlineStatus->setObjectName(QStringLiteral("analysisOfflineStatus")); m_offlineStatus->setProperty("role", "caption"); m_offlineStatus->setWordWrap(true);
    offlineLayout->addWidget(m_offlineStatus, 3, 0, 1, 4); offlineLayout->setColumnStretch(1, 1); root->addWidget(offline); offline->hide();
    connect(offlineToggle, &QPushButton::toggled, this, [offline, offlineToggle](bool visible) {
        offline->setVisible(visible); offlineToggle->setText(visible ? QStringLiteral("整段录制分析 ▴") : QStringLiteral("整段录制分析 ▾"));
    });
    // Save dialogs deliberately select a NEW directory name. The archive service
    // rejects existing paths; this widget never makes or overwrites a directory.
    const auto newDirectory = [this](QLineEdit *edit, const QString &title) {
        const QString path = QFileDialog::getSaveFileName(this, title, edit->text(), QString(), nullptr, QFileDialog::DontConfirmOverwrite);
        if (!path.isEmpty()) edit->setText(path);
    };
    connect(choose, &QPushButton::clicked, this, [=]() { newDirectory(m_archivePath, QStringLiteral("指定新的事件归档目录")); });
    connect(outputChoose, &QPushButton::clicked, this, [=]() { newDirectory(m_outputPath, QStringLiteral("指定新的整段分析输出目录")); });
    connect(sourceChoose, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择录制目录"), m_recordingPath->text());
        if (!path.isEmpty()) m_recordingPath->setText(path);
    });
    connect(open, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("打开事件归档"), m_archivePath->text());
        if (!path.isEmpty()) emit openArchiveRequested(path);
    });
    connect(rule, &QPushButton::clicked, this, &SpikeAnalysisControls::ruleEditorRequested);
    connect(m_start, &QPushButton::clicked, this, [this]() { emit startArchiveRequested(m_archivePath->text().trimmed()); });
    connect(m_stop, &QPushButton::clicked, this, &SpikeAnalysisControls::stopArchiveRequested);
    connect(m_analyze, &QPushButton::clicked, this, [this]() { emit analyzeRecordingRequested(m_recordingPath->text().trimmed(), m_outputPath->text().trimmed()); });
    connect(m_cancel, &QPushButton::clicked, this, &SpikeAnalysisControls::cancelAnalysisRequested);
    for (auto *edit : {m_archivePath, m_recordingPath, m_outputPath}) connect(edit, &QLineEdit::textChanged, this, &SpikeAnalysisControls::updateAvailability);
    updateAvailability();
}
void SpikeAnalysisControls::setArchiveDestination(const QString &directory) { m_archivePath->setText(directory); }
void SpikeAnalysisControls::setOfflinePaths(const QString &recording, const QString &output) { m_recordingPath->setText(recording); m_outputPath->setText(output); }
void SpikeAnalysisControls::updateAvailability() {
    m_start->setEnabled(!m_archiveRunning && !m_archivePath->text().trimmed().isEmpty()); m_stop->setEnabled(m_archiveCanStop);
    m_archivePath->setEnabled(!m_archiveRunning); m_chooseDestination->setEnabled(!m_archiveRunning);
    m_analyze->setEnabled(!m_offlineRunning && !m_recordingPath->text().trimmed().isEmpty() && !m_outputPath->text().trimmed().isEmpty());
    m_cancel->setEnabled(m_offlineCancellable); m_recordingPath->setEnabled(!m_offlineRunning); m_outputPath->setEnabled(!m_offlineRunning);
    m_chooseRecording->setEnabled(!m_offlineRunning); m_chooseOutput->setEnabled(!m_offlineRunning);
}
void SpikeAnalysisControls::setArchiveStatus(const SpikeArchiveStatus &status, const QString &directory) {
    m_archiveCanStop = status.state == SpikeArchiveState::Running;
    m_archiveRunning = status.state == SpikeArchiveState::Running ||
        (status.state == SpikeArchiveState::Failed && !status.writerFinished);
    if (!directory.isEmpty()) {
        if (m_archiveRunning || directory != m_reportedArchiveDirectory || m_archivePath->text().trimmed().isEmpty())
            m_archivePath->setText(directory);
        m_reportedArchiveDirectory = directory;
    }
    const auto count = [&](const char *key) { bool ok = false; const auto value = status.sourceIntegrity.value(QLatin1String(key)).toVariant().toULongLong(&ok); return ok ? QString::number(value) : QStringLiteral("?"); };
    const QString quality = status.sourceIntegrity.isEmpty() ? QStringLiteral("源完整性尚未报告；未知不等于无缺口")
        : QStringLiteral("源：缺失 %1 / 队列丢失 %2 / 无效 %3 / 未验证 %4 / 待窗 %5 / 边界排除 %6")
            .arg(count("missing_source_frames"), count("queue_dropped_frames"), count("invalid_frames"), count("unverified_frames"), count("pending_window_events"), count("boundary_excluded_events"));
    m_archiveStatus->setText(QStringLiteral("%1 · 已写 %2 / 已接受 %3 · 队列 %4 KiB%5%6\n%7\n实时归档只含开始后发出的完整窗口，不包含更早事件或保证全会话覆盖")
        .arg(lifecycle(status.state) + (status.state == SpikeArchiveState::Failed && !status.writerFinished ? QStringLiteral("；仍在结束写入，请等待") : QString())).arg(status.writtenEvents).arg(status.acceptedEvents).arg(status.queuedBytes / 1024)
        .arg(status.finishVerified ? QStringLiteral(" · 终态已验证") : QString())
        .arg(status.error.isEmpty() ? QString() : QStringLiteral(" · ") + status.error).arg(quality));
    updateAvailability();
}
void SpikeAnalysisControls::setOfflineProgress(quint64 completed, quint64 total, const QString &status, bool cancellable, bool finalizing) {
    m_offlineRunning = cancellable || finalizing; m_offlineCancellable = cancellable;
    if (m_offlineRunning && total == 0) { m_progress->setRange(0, 0); }
    else {
        m_progress->setRange(0, 1000);
        m_progress->setValue(total ? int(std::min<long double>(1000.0L, 1000.0L * completed / total)) : 0);
    }
    m_offlineStatus->setText(status); updateAvailability();
}
void SpikeAnalysisControls::setWaveTheme(const QMap<QString, QString> &palette) {
    const QColor bg(palette.value(QStringLiteral("appBg"))); if (bg.isValid()) m_windowBg = bg;
    const auto color = [&](const char *key, const char *fallback) { const QColor c(palette.value(QLatin1String(key))); return c.isValid() ? c.name() : QString::fromLatin1(fallback); };
    m_progress->setStyleSheet(QStringLiteral("QProgressBar { background: %1; color: %2; border: 1px solid %3; border-radius: 4px; text-align: center; min-height: 18px; } QProgressBar::chunk { background: %4; border-radius: 3px; }")
        .arg(color("plotBg", "#131518"), color("text", "#E8EAED"), color("border", "#31363D"), color("accent", "#4D8FE8")));
    update();
}
void SpikeAnalysisControls::paintEvent(QPaintEvent *) { QPainter p(this); p.fillRect(rect(), m_windowBg); }
} // namespace ccv2
