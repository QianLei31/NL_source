#include "ui/widgets/spike_archive_browser.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace ccv2 {
struct SpikeArchiveBrowser::Async {
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
    quint64 generation = 0;
    bool busy = false;
};
namespace {
struct ArchiveJobResult {
    SpikeEventArchiveReader reader;
    SpikeArchivePage page;
    SpikeArchiveAnnotationPage annotations;
    SpikeArchiveAnnotation last;
    QString error;
    bool success = false;
    bool committed = false;
};
// The worker owns only copied value objects and its cancellation token. The
// parent-owned watcher safely disconnects on widget destruction; its destructor
// does not wait for an outstanding verification. No worker touches a QWidget.
template<class Work, class Done>
void archiveJob(SpikeArchiveBrowser *owner, Work work, Done done) {
    auto *watcher = new QFutureWatcher<ArchiveJobResult>(owner);
    QObject::connect(watcher, &QFutureWatcher<ArchiveJobResult>::finished, owner, [watcher, done = std::move(done)]() mutable {
        auto result = watcher->result(); watcher->deleteLater(); done(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}
QString n(double x) { return std::isfinite(x) ? QString::number(x, 'g', 7) : QStringLiteral("n/a"); }
QString classification(SpikeClassificationStatus s) {
    switch (s) {
    case SpikeClassificationStatus::Unassigned: return QStringLiteral("未分配");
    case SpikeClassificationStatus::Assigned: return QStringLiteral("规则匹配");
    case SpikeClassificationStatus::Ambiguous: return QStringLiteral("多候选歧义");
    case SpikeClassificationStatus::Incompatible: return QStringLiteral("上下文不兼容");
    case SpikeClassificationStatus::Manual: return QStringLiteral("人工候选");
    }
    return QStringLiteral("未知");
}
QString lifecycle(SpikeArchiveState s) {
    switch (s) {
    case SpikeArchiveState::Idle: return QStringLiteral("未开始");
    case SpikeArchiveState::Running: return QStringLiteral("未完成 / 运行中前缀");
    case SpikeArchiveState::Completed: return QStringLiteral("档案写入完成");
    case SpikeArchiveState::Drained: return QStringLiteral("停止并排空");
    case SpikeArchiveState::Cancelled: return QStringLiteral("已取消");
    case SpikeArchiveState::Failed: return QStringLiteral("失败");
    }
    return {};
}
QPushButton *button(const QString &text, const char *name) {
    auto *b = new QPushButton(text); b->setObjectName(QLatin1String(name)); b->setProperty("variant", "secondary"); return b;
}
QLabel *caption(const char *name) {
    auto *l = new QLabel; l->setObjectName(QLatin1String(name)); l->setProperty("role", "caption"); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l;
}
}

class SpikeArchiveWaveform : public QWidget {
public:
    explicit SpikeArchiveWaveform(SpikeArchiveBrowser *owner) : QWidget(owner), m_owner(owner) {
        setObjectName(QStringLiteral("archiveSelectedWaveform")); setMinimumHeight(185);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(QStringLiteral("所选归档事件的输入等效波形"));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(m_owner->m_border); p.setBrush(m_owner->m_plotBg); p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);
        p.setBrush(Qt::NoBrush); p.setPen(m_owner->m_text);
        p.drawText(QRect(12, 7, width() - 24, 20), QStringLiteral("所选事件波形 · 原始自动分类保持不变"));
        const QRectF area(64, 35, std::max(1, width() - 86), std::max(1, height() - 83));
        p.setPen(m_owner->m_axis); p.drawRect(area);
        p.drawText(QRectF(area.left(), height() - 22, area.width(), 18), Qt::AlignCenter, QStringLiteral("相对阈值时间 (ms)"));
        p.save(); p.translate(13, area.center().y()); p.rotate(-90);
        p.drawText(QRectF(-area.height() / 2, -9, area.height(), 18), Qt::AlignCenter, QStringLiteral("输入等效 µV")); p.restore();
        const int row = m_owner->m_table->currentRow();
        if (row < 0 || row >= m_owner->m_page.events.size()) {
            p.setPen(m_owner->m_secondary); p.drawText(area, Qt::AlignCenter, QStringLiteral("选择表中事件以查看波形")); return;
        }
        const auto &r = m_owner->m_page.events[row]; const auto &e = r.event;
        if (r.waveform.size() < 2 || !std::isfinite(e.sourceSampleRate) || e.sourceSampleRate <= 0 || e.sampleStride < 1) return;
        const double step = 1000.0 * e.sampleStride / e.sourceSampleRate;
        const double lo = -e.preSamples * step, hi = (r.waveform.size() - 1 - e.preSamples) * step;
        double amplitude = 1;
        for (float v : r.waveform) if (std::isfinite(v)) amplitude = std::max(amplitude, std::abs(double(v) * 1e6) * 1.1);
        const auto point = [&](double x, double y) { return QPointF(area.left() + (x - lo) / (hi - lo) * area.width(), area.center().y() - y / amplitude * area.height() / 2); };
        p.setPen(m_owner->m_axis);
        for (int i = 0; i < 3; ++i) {
            const double t = lo + (hi - lo) * i / 2;
            p.drawText(QRectF(point(t, 0).x() - 35, area.bottom() + 3, 70, 16), Qt::AlignCenter, n(t));
            const double v = amplitude * (i - 1);
            p.drawText(QRectF(16, point(lo, v).y() - 8, 43, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'g', 4));
        }
        p.save(); p.setClipRect(area); p.setPen(QPen(m_owner->m_border, 1, Qt::DashLine));
        p.drawLine(point(0, -amplitude), point(0, amplitude)); p.drawLine(point(lo, 0), point(hi, 0));
        QPainterPath path; bool start = true;
        for (int i = 0; i < r.waveform.size(); ++i) {
            if (!std::isfinite(r.waveform[i])) { start = true; continue; }
            const QPointF pos = point((i - e.preSamples) * step, r.waveform[i] * 1e6);
            if (start) { path.moveTo(pos); start = false; } else path.lineTo(pos);
        }
        p.setPen(QPen(m_owner->m_wave, 1.7)); p.drawPath(path); p.restore();
    }
private:
    SpikeArchiveBrowser *m_owner;
};

SpikeArchiveBrowser::SpikeArchiveBrowser(QWidget *parent) : QWidget(parent, Qt::Window), m_async(std::make_unique<Async>()) {
    setObjectName(QStringLiteral("spikeArchiveBrowser")); setWindowTitle(QStringLiteral("事件归档 · 历史浏览"));
    setAttribute(Qt::WA_QuitOnClose, false); resize(1100, 810); setMinimumSize(760, 690);
    auto *root = new QVBoxLayout(this); root->setContentsMargins(12, 12, 12, 12); root->setSpacing(7);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("事件归档")); title->setObjectName(QStringLiteral("title")); heading->addWidget(title);
    auto *qualification = caption("archiveQualification"); qualification->setText(QStringLiteral("分页读取已验证记录 · 生命周期与源完整性分别报告 · 候选标签不代表单神经元分离"));
    heading->addWidget(qualification, 1); root->addLayout(heading);
    auto *panel = new QWidget; panel->setProperty("role", "page-control-panel");
    auto *controls = new QVBoxLayout(panel); controls->setContentsMargins(0, 0, 0, 0); controls->setSpacing(6);
    auto *pathRow = new QHBoxLayout; m_path = new QLineEdit; m_path->setObjectName(QStringLiteral("archivePath")); m_path->setPlaceholderText(QStringLiteral("事件归档 sidecar 目录")); pathRow->addWidget(m_path, 1);
    auto *browse = button(QStringLiteral("选择…"), "archiveBrowse"); auto *open = button(QStringLiteral("打开 / 重新验证"), "archiveOpen");
    pathRow->addWidget(browse); pathRow->addWidget(open); controls->addLayout(pathRow);
    auto *filters = new QGridLayout; filters->setSpacing(6);
    m_electrode = new QSpinBox; m_electrode->setObjectName(QStringLiteral("archiveElectrode")); m_electrode->setRange(-1, 1023); m_electrode->setSpecialValueText(QStringLiteral("全部")); m_electrode->setValue(-1);
    m_epochEnabled = new QCheckBox(QStringLiteral("epoch")); m_epochEnabled->setObjectName(QStringLiteral("archiveEpochEnabled"));
    m_epoch = new QLineEdit(QStringLiteral("0")); m_epoch->setObjectName(QStringLiteral("archiveEpoch")); m_epoch->setMaximumWidth(150); m_epoch->setEnabled(false);
    m_epoch->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{1,20}")), m_epoch));
    m_from = new QDoubleSpinBox; m_to = new QDoubleSpinBox;
    for (auto *v : {m_from, m_to}) { v->setRange(-1, 1e12); v->setDecimals(6); v->setSpecialValueText(QStringLiteral("不限")); v->setValue(-1); v->setSuffix(QStringLiteral(" s")); }
    m_from->setObjectName(QStringLiteral("archiveTimeFrom")); m_to->setObjectName(QStringLiteral("archiveTimeTo"));
    auto *apply = button(QStringLiteral("应用筛选"), "archiveFilter");
    filters->addWidget(new QLabel(QStringLiteral("物理电极")), 0, 0); filters->addWidget(m_electrode, 0, 1); filters->addWidget(m_epochEnabled, 0, 2); filters->addWidget(m_epoch, 0, 3);
    filters->addWidget(new QLabel(QStringLiteral("源时间从")), 1, 0); filters->addWidget(m_from, 1, 1); filters->addWidget(new QLabel(QStringLiteral("至")), 1, 2); filters->addWidget(m_to, 1, 3); filters->addWidget(apply, 1, 4); filters->setColumnStretch(3, 1); controls->addLayout(filters); root->addWidget(panel);
    m_status = caption("archiveStatus"); root->addWidget(m_status);
    auto *splitter = new QSplitter(Qt::Vertical); splitter->setChildrenCollapsible(false);
    m_table = new QTableWidget(0, 10); m_table->setObjectName(QStringLiteral("archiveEvents"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("事件 ID"), QStringLiteral("电极 / 相位"), QStringLiteral("epoch / 段"), QStringLiteral("源时间 s"), QStringLiteral("源帧"), QStringLiteral("自动候选"), QStringLiteral("分类状态"), QStringLiteral("检测版本"), QStringLiteral("规则版本"), QStringLiteral("质量")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(false); m_table->verticalHeader()->hide(); m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); m_table->horizontalHeader()->setStretchLastSection(true); m_table->setMinimumHeight(120);
    splitter->addWidget(m_table); m_waveform = new SpikeArchiveWaveform(this); splitter->addWidget(m_waveform); splitter->setSizes({260, 230}); root->addWidget(splitter, 1);
    auto *pagingPanel = new QWidget; pagingPanel->setProperty("role", "page-control-panel"); auto *paging = new QHBoxLayout(pagingPanel); paging->setContentsMargins(0, 0, 0, 0); m_previous = button(QStringLiteral("上一页"), "archivePrevious"); m_next = button(QStringLiteral("下一页 / 继续扫描"), "archiveNext"); m_paging = caption("archivePaging");
    paging->addWidget(m_previous); paging->addWidget(m_next); paging->addWidget(m_paging, 1); root->addWidget(pagingPanel);
    auto *annotationPanel = new QWidget; annotationPanel->setProperty("role", "page-control-panel");
    auto *annotationLayout = new QVBoxLayout(annotationPanel); annotationLayout->setContentsMargins(0, 0, 0, 0); annotationLayout->setSpacing(6);
    auto *annotationRow = new QHBoxLayout; m_unit = new QSpinBox; m_unit->setObjectName(QStringLiteral("archiveLabelUnit")); m_unit->setRange(1, 32); m_unit->setPrefix(QStringLiteral("候选 "));
    m_note = new QLineEdit; m_note->setObjectName(QStringLiteral("archiveLabelNote")); m_note->setMaxLength(1000); m_note->setPlaceholderText(QStringLiteral("人工标签说明（追加保存，不改变自动分类）"));
    m_label = button(QStringLiteral("追加标签"), "archiveAppendLabel"); m_label->setProperty("variant", "primary"); m_unlabel = button(QStringLiteral("追加取消标记"), "archiveRemoveLabel");
    annotationRow->addWidget(m_unit); annotationRow->addWidget(m_note, 1); annotationRow->addWidget(m_label); annotationRow->addWidget(m_unlabel); annotationLayout->addLayout(annotationRow);
    auto *historyRow = new QHBoxLayout; m_annotation = caption("archiveAnnotationHistory"); m_moreAnnotations = button(QStringLiteral("后续标签记录"), "archiveMoreAnnotations"); historyRow->addWidget(m_annotation, 1); historyRow->addWidget(m_moreAnnotations); annotationLayout->addLayout(historyRow); root->addWidget(annotationPanel);
    auto *detailPanel = new QWidget; detailPanel->setProperty("role", "page-control-panel"); auto *detailRow = new QHBoxLayout(detailPanel); detailRow->setContentsMargins(0, 0, 0, 0); auto *detailToggle = button(QStringLiteral("运行与源完整性详情 ▾"), "archiveDetailsToggle"); detailToggle->setCheckable(true); detailRow->addWidget(detailToggle); detailRow->addStretch(); m_cancel = button(QStringLiteral("取消读取"), "archiveCancelRead"); m_cancel->setEnabled(false); detailRow->addWidget(m_cancel); root->addWidget(detailPanel);
    m_details = caption("archiveDetails"); m_details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto *detailsViewport = new QScrollArea; detailsViewport->setObjectName(QStringLiteral("archiveDetailsViewport")); detailsViewport->setWidgetResizable(true); detailsViewport->setFrameShape(QFrame::NoFrame); detailsViewport->setMinimumHeight(80); detailsViewport->setMaximumHeight(120); detailsViewport->setWidget(m_details); root->addWidget(detailsViewport); detailsViewport->hide();
    m_feedback = caption("archiveFeedback"); root->addWidget(m_feedback);
    connect(detailToggle, &QPushButton::toggled, this, [this, detailsViewport](bool visible) { detailsViewport->setVisible(visible); setMinimumHeight(visible ? 790 : 690); });
    connect(m_cancel, &QPushButton::clicked, this, &SpikeArchiveBrowser::cancelPending);
    connect(m_epochEnabled, &QCheckBox::toggled, m_epoch, &QWidget::setEnabled);
    connect(browse, &QPushButton::clicked, this, [this]() { const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("打开事件归档"), m_path->text()); if (!path.isEmpty()) openArchive(path); });
    connect(open, &QPushButton::clicked, this, [this]() { openArchive(m_path->text().trimmed()); });
    connect(m_path, &QLineEdit::returnPressed, open, &QPushButton::click);
    connect(apply, &QPushButton::clicked, this, &SpikeArchiveBrowser::resetQuery);
    connect(m_next, &QPushButton::clicked, this, [this]() { if (m_page.atEnd || !m_page.error.isEmpty()) return; m_history.append(m_cursor); if (m_history.size() > 128) m_history.removeFirst(); showPage(m_page.next); });
    connect(m_previous, &QPushButton::clicked, this, [this]() { if (!m_history.isEmpty()) showPage(m_history.takeLast()); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &SpikeArchiveBrowser::selectEvent);
    connect(m_label, &QPushButton::clicked, this, [this]() { appendSelectedLabel(m_unit->value(), m_note->text()); });
    connect(m_unlabel, &QPushButton::clicked, this, [this]() { appendSelectedLabel(-1, m_note->text()); });
    connect(m_moreAnnotations, &QPushButton::clicked, this, [this]() { showAnnotations(m_annotationNext); });
    refreshStatus(); selectEvent();
}

SpikeArchiveBrowser::~SpikeArchiveBrowser() { m_async->cancel->store(true); }
bool SpikeArchiveBrowser::busy() const { return m_async->busy; }
void SpikeArchiveBrowser::setBusy(bool busy) {
    const bool changed = m_async->busy != busy; m_async->busy = busy;
    m_path->parentWidget()->setEnabled(!busy); m_table->setEnabled(!busy);
    m_previous->setEnabled(!busy && !m_history.isEmpty()); m_next->setEnabled(!busy && m_open && !m_page.atEnd && m_page.error.isEmpty());
    const bool canLabel = !busy && selectedEventId() && m_open && m_page.error.isEmpty() && m_reader.status().finishVerified;
    m_label->setEnabled(canLabel); m_unlabel->setEnabled(canLabel); m_note->setEnabled(!busy); m_unit->setEnabled(!busy);
    m_moreAnnotations->setEnabled(false); m_cancel->setEnabled(busy);
    if (changed) emit busyChanged(busy);
}
quint64 SpikeArchiveBrowser::beginOperation(const QString &status) {
    m_async->cancel->store(true); m_async->cancel = std::make_shared<std::atomic_bool>(false);
    ++m_async->generation; setBusy(true); m_feedback->setText(status); return m_async->generation;
}
void SpikeArchiveBrowser::cancelPending() {
    if (!busy()) return;
    m_async->cancel->store(true); ++m_async->generation; setBusy(false);
    if (m_open) m_path->setText(m_reader.directory());
    m_feedback->setText(QStringLiteral("已取消等待，旧结果不会替换当前页面；若标签写入已经完成，不会撤销，请重新打开核对"));
}
void SpikeArchiveBrowser::closeEvent(QCloseEvent *event) { cancelPending(); QWidget::closeEvent(event); }
bool SpikeArchiveBrowser::openArchive(const QString &directory) {
    if (directory.trimmed().isEmpty()) { m_feedback->setText(QStringLiteral("请选择事件归档目录")); return false; }
    const quint64 generation = beginOperation(QStringLiteral("正在校验归档；可取消读取，界面保持响应…"));
    const auto cancel = m_async->cancel;
    archiveJob(this, [directory, cancel]() {
        ArchiveJobResult result; result.success = result.reader.open(directory, &result.error, cancel.get()); return result;
    }, [this, generation, directory](ArchiveJobResult result) {
        if (generation != m_async->generation) return;
        setBusy(false);
        if (!result.success) { if (m_open) m_path->setText(m_reader.directory()); m_feedback->setText(QStringLiteral("无法打开指定归档：%1%2").arg(result.error, m_open ? QStringLiteral("；仍显示此前已验证归档") : QString())); emit archiveOpened(false); return; }
        m_reader = std::move(result.reader); m_open = true; m_path->setText(directory); m_feedback->clear(); refreshStatus(); resetQuery(); emit archiveOpened(true);
    });
    return true;
}
SpikeArchiveQuery SpikeArchiveBrowser::query() const {
    SpikeArchiveQuery q; q.electrode = m_electrode->value(); q.filterEpoch = m_epochEnabled->isChecked(); q.epoch = m_epoch->text().toULongLong(); q.sourceTimeFrom = m_from->value(); q.sourceTimeTo = m_to->value(); return q;
}
void SpikeArchiveBrowser::resetQuery() {
    if (!m_open) return;
    bool epochOk = false; m_epoch->text().toULongLong(&epochOk);
    if (m_epochEnabled->isChecked() && !epochOk) { m_feedback->setText(QStringLiteral("epoch 必须是有效的 64 位非负整数")); return; }
    if (m_from->value() >= 0 && m_to->value() >= 0 && m_from->value() > m_to->value()) { m_feedback->setText(QStringLiteral("源时间结束必须不早于开始")); return; }
    m_activeQuery = query(); m_history.clear(); m_feedback->clear(); showPage({});
}
void SpikeArchiveBrowser::showPage(SpikeArchiveCursor cursor) {
    if (!m_open) return;
    const quint64 generation = beginOperation(QStringLiteral("正在读取下一段已验证事件…"));
    const auto cancel = m_async->cancel; const auto reader = m_reader; const auto query = m_activeQuery;
    archiveJob(this, [reader, query, cursor, cancel]() {
        ArchiveJobResult result;
        if (!cancel->load()) result.page = reader.readPage(query, cursor, 128, 4 * 1024 * 1024);
        return result;
    }, [this, generation, cursor](ArchiveJobResult result) {
        if (generation != m_async->generation) return;
        setBusy(false); m_feedback->clear(); applyPage(cursor, std::move(result.page)); emit pageReady();
    });
}
void SpikeArchiveBrowser::applyPage(SpikeArchiveCursor cursor, SpikeArchivePage page) {
    m_cursor = cursor; m_page = std::move(page);
    const QSignalBlocker blocker(m_table); m_table->clearContents(); m_table->setRowCount(int(m_page.events.size()));
    for (int row = 0; row < m_page.events.size(); ++row) {
        const auto &r = m_page.events[row]; const auto &e = r.event;
        bool finite = !r.waveform.isEmpty(); for (float v : r.waveform) finite = finite && std::isfinite(v);
        const QString quality = (!e.hasSourceTime() ? QStringLiteral("时间未知") : !finite ? QStringLiteral("波形含无效值") : QStringLiteral("波形有限；源质量见详情"));
        const QStringList values = {QString::number(e.eventId), QStringLiteral("%1 / %2").arg(e.electrode).arg(e.tdmPhase), QStringLiteral("%1 / %2").arg(e.epoch).arg(e.continuitySegment), e.hasSourceTime() ? n(e.sourceTimeSeconds()) : QStringLiteral("n/a"), QString::number(e.sourceFrame), e.unitId > 0 ? QString::number(e.unitId) : QStringLiteral("—"), classification(e.classification), QString::number(e.detectorRevision), QString::number(e.ruleRevision), quality};
        for (int col = 0; col < values.size(); ++col) { auto *item = new QTableWidgetItem(values[col]); item->setToolTip(values[col] + QStringLiteral("\nrun ") + e.runId.toString(QUuid::WithoutBraces)); m_table->setItem(row, col, item); }
    }
    m_previous->setEnabled(!m_history.isEmpty()); m_next->setEnabled(!m_page.atEnd && m_page.error.isEmpty());
    m_paging->setText(QStringLiteral("本页 %1 条 / 全归档 %2 条 · 每页 ≤128 · %3").arg(m_page.events.size()).arg(m_reader.status().writtenEvents).arg(m_page.atEnd ? QStringLiteral("已到已验证记录末尾") : QStringLiteral("仍可继续；空页不表示无后续事件")));
    if (!m_page.error.isEmpty()) m_feedback->setText(QStringLiteral("读取失败：%1").arg(m_page.error));
    if (m_table->rowCount()) m_table->setCurrentCell(0, 0); else m_table->setCurrentCell(-1, -1);
    selectEvent();
}
quint64 SpikeArchiveBrowser::selectedEventId() const { const int row = m_table->currentRow(); return row >= 0 && row < m_page.events.size() ? m_page.events[row].event.eventId : 0; }
void SpikeArchiveBrowser::selectEvent() {
    const bool selected = selectedEventId() != 0 && m_page.error.isEmpty(); const bool terminal = m_open && m_reader.status().finishVerified;
    m_label->setEnabled(selected && terminal); m_unlabel->setEnabled(selected && terminal);
    m_label->setToolTip(terminal ? QStringLiteral("追加持久人工注释；自动候选保持不变") : QStringLiteral("终态验证后才可追加；活动归档请通过分析服务标记"));
    m_waveform->update(); if (m_page.error.isEmpty()) showAnnotations();
}
void SpikeArchiveBrowser::showAnnotations(qint64 offset) {
    m_moreAnnotations->setEnabled(false);
    const quint64 id = selectedEventId(); if (!id) { m_annotation->setText(QStringLiteral("未选择事件")); return; }
    const quint64 generation = beginOperation(QStringLiteral("正在读取所选事件的人工标签记录…"));
    const auto cancel = m_async->cancel; const auto reader = m_reader;
    archiveJob(this, [reader, id, offset, cancel]() {
        ArchiveJobResult result; if (!cancel->load()) result.annotations = reader.readAnnotations(id, offset, 128); return result;
    }, [this, generation](ArchiveJobResult result) {
        if (generation != m_async->generation) return;
        setBusy(false); m_feedback->clear(); displayAnnotations(result.annotations);
    });
}
void SpikeArchiveBrowser::displayAnnotations(const SpikeArchiveAnnotationPage &page) {
    m_annotationNext = page.nextOffset;
    QStringList rows;
    // The history page stays bounded; only the latest four entries in this page
    // are painted. More pages remain explicitly available via the cursor button.
    for (int i = std::max(0, int(page.annotations.size()) - 4); i < page.annotations.size(); ++i) {
        const auto &a = page.annotations[i]; rows << QStringLiteral("#%1 · %2 · %3%4").arg(a.annotationId).arg(a.utc, a.unitId < 0 ? QStringLiteral("取消人工标记") : QStringLiteral("人工候选 %1").arg(a.unitId), a.note.isEmpty() ? QString() : QStringLiteral(" · ") + a.note.left(150));
    }
    if (rows.isEmpty()) rows << (page.atEnd ? QStringLiteral("当前扫描页无人工标签记录") : QStringLiteral("当前扫描页无标签；可继续扫描"));
    if (!page.error.isEmpty()) rows << QStringLiteral("标签读取：") + page.error;
    if (page.annotations.size() > 4) rows.prepend(QStringLiteral("本页 %1 条；下方显示最后4条").arg(page.annotations.size()));
    m_annotation->setText(rows.join(QLatin1Char('\n'))); m_moreAnnotations->setEnabled(!page.atEnd && page.error.isEmpty());
}
bool SpikeArchiveBrowser::appendSelectedLabel(int unitId, const QString &note) {
    const quint64 id = selectedEventId(); if (busy() || !id || !m_page.error.isEmpty() || !m_reader.status().finishVerified) return false;
    if ((unitId != -1 && (unitId < 1 || unitId > 32)) || note.size() > 1000) return false;
    const QUuid run = m_reader.status().runId; const QString directory = m_reader.directory();
    const quint64 generation = beginOperation(QStringLiteral("正在追加人工标签并重新打开校验；原自动分类保持不变…"));
    const auto cancel = m_async->cancel;
    archiveJob(this, [directory, run, id, unitId, note, cancel]() {
        ArchiveJobResult result;
        result.committed = SpikeEventArchiveReader::appendManualLabel(directory, run, id, unitId, note, &result.error, cancel.get());
        if (!result.committed || cancel->load()) return result;
        if (!result.reader.open(directory, &result.error, cancel.get())) return result;
        qint64 cursor = 32; bool seen = false;
        while (!cancel->load()) {
            const auto page = result.reader.readAnnotations(id, cursor, 128);
            if (!page.error.isEmpty()) { result.error = page.error; return result; }
            for (const auto &annotation : page.annotations) { result.last = annotation; seen = true; }
            if (page.atEnd) break;
            if (page.nextOffset == cursor) { result.error = QStringLiteral("标签游标未前进"); return result; }
            cursor = page.nextOffset;
        }
        result.success = !cancel->load() && seen && result.last.runId == run && result.last.eventId == id && result.last.unitId == unitId && result.last.note == note;
        return result;
    }, [this, generation, run, id, unitId, note](ArchiveJobResult result) {
        if (generation != m_async->generation) return;
        setBusy(false);
        if (result.success) {
            m_reader = std::move(result.reader); m_note->clear();
            m_feedback->setText(QStringLiteral("已追加标签 #%1，并由重新打开的归档验证 · run %2 / event %3 · 自动分类未改动").arg(result.last.annotationId).arg(run.toString(QUuid::WithoutBraces)).arg(id));
            m_annotation->setText(QStringLiteral("最新已验证：#%1 · %2 · %3%4").arg(result.last.annotationId).arg(result.last.utc, unitId < 0 ? QStringLiteral("取消人工标记") : QStringLiteral("人工候选 %1").arg(unitId), note.isEmpty() ? QString() : QStringLiteral(" · ") + note));
        } else m_feedback->setText(result.committed ? QStringLiteral("标签已追加，但重开校验未确认：%1").arg(result.error.isEmpty() ? QStringLiteral("最终标签不匹配；可能存在并发追加") : result.error) : QStringLiteral("标签未保存：%1").arg(result.error));
        emit annotationFinished(result.success);
    });
    return true;
}
void SpikeArchiveBrowser::refreshStatus() {
    if (!m_open) { m_status->setText(QStringLiteral("选择归档以读取其已验证事件历史；归档范围与源覆盖单独报告")); m_previous->setEnabled(false); m_next->setEnabled(false); return; }
    const auto s = m_reader.status();
    const auto count = [&](const char *key) { const auto value = s.sourceIntegrity.value(QLatin1String(key)); bool ok = false; const auto number = value.toVariant().toULongLong(&ok); return ok ? QString::number(number) : QStringLiteral("?"); };
    const QString sourceSummary = s.sourceIntegrity.isEmpty() ? QStringLiteral("源完整性未提供")
        : QStringLiteral("源质量：缺失 %1 / 队列丢失 %2 / 无效 %3 / 未验证 %4 / 待窗 %5 / 边界排除 %6%7")
            .arg(count("missing_source_frames"), count("queue_dropped_frames"), count("invalid_frames"), count("unverified_frames"), count("pending_window_events"), count("boundary_excluded_events"))
            .arg(s.sourceIntegrity.value(QStringLiteral("stopped_early")).toBool() ? QStringLiteral(" / 提前停止") : QString());
    const auto metadata = m_reader.runMetadata();
    const QString scope = metadata.value(QStringLiteral("event_scope")).toString() == QStringLiteral("immutable_complete_windows_emitted_after_archive_start")
        ? QStringLiteral("范围：仅归档开始后发出的完整窗口；不包含更早事件，不宣称全会话覆盖")
        : metadata.value(QStringLiteral("analysis_mode")).toString() == QStringLiteral("dedicated_lossless_offline")
            ? QStringLiteral("范围：独立整段录制分析；已写完不表示源无缺口或所有窗口均有效")
            : QStringLiteral("范围：以运行元数据为准；没有全源覆盖保证");
    m_status->setText(QStringLiteral("%1 · 已验证 %2 条事件 · %3%4%5\n%6\n%7").arg(lifecycle(s.state)).arg(s.writtenEvents).arg(s.finishVerified ? QStringLiteral("终态已验证") : QStringLiteral("无已验证终态，不能视为完成"), s.recoveredPrefix ? QStringLiteral(" · 已恢复有效前缀") : QString(), s.error.isEmpty() ? QString() : QStringLiteral(" · ") + s.error, sourceSummary, scope));
    const QString integrity = s.sourceIntegrity.isEmpty() ? QStringLiteral("未提供源完整性统计；不可据此声称没有缺口") : QString::fromUtf8(QJsonDocument(s.sourceIntegrity).toJson(QJsonDocument::Compact));
    m_details->setText(QStringLiteral("run %1\n归档生命周期：%2；源完整性单独列出，完成不等于源无缺口。\n源完整性：%3\n运行元数据：%4\n源时间只在对应 epoch 内有意义；不同 epoch 不自动合并。历史回退保留最近128个页游标。").arg(s.runId.toString(QUuid::WithoutBraces), lifecycle(s.state), integrity.left(8000), QString::fromUtf8(QJsonDocument(m_reader.runMetadata()).toJson(QJsonDocument::Compact)).left(8000)));
}
void SpikeArchiveBrowser::setWaveTheme(const QMap<QString, QString> &palette) {
    const auto pick = [&](const char *key, QColor &c) { const QColor value(palette.value(QLatin1String(key))); if (value.isValid()) c = value; };
    pick("appBg", m_windowBg); pick("plotBg", m_plotBg); pick("border", m_border); pick("text", m_text); pick("title", m_secondary); pick("axis", m_axis); pick("wave", m_wave); pick("accent", m_accent);
    update(); m_waveform->update();
}
void SpikeArchiveBrowser::paintEvent(QPaintEvent *) { QPainter p(this); p.fillRect(rect(), m_windowBg); }
} // namespace ccv2
