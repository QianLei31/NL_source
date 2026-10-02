#include "ui/widgets/spike_rule_editor.h"

#include <QAction>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace ccv2 {
namespace {
QString n(double value) { return std::isfinite(value) ? QString::number(value, 'g', 6) : QStringLiteral("n/a"); }
QPushButton *button(const QString &text, const char *name) { auto *b = new QPushButton(text); b->setObjectName(QLatin1String(name)); b->setProperty("variant", "secondary"); return b; }
QLabel *caption(const char *name) { auto *l = new QLabel; l->setObjectName(QLatin1String(name)); l->setProperty("role", "caption"); l->setWordWrap(true); l->setTextFormat(Qt::PlainText); return l; }
bool sameDefinitions(const QVector<SpikeElectrodeRules> &a, const QVector<SpikeElectrodeRules> &b) {
    const auto sa = SpikeRuleSet::create(1, a), sb = SpikeRuleSet::create(1, b);
    return sa && sb && sa->toJson() == sb->toJson();
}
}

class SpikeRulePlot : public QWidget {
public:
    explicit SpikeRulePlot(SpikeRuleEditor *owner) : QWidget(owner), m_owner(owner) {
        setObjectName(QStringLiteral("ruleWaveformPlot")); setMinimumSize(340, 250);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("波形框规则画布"));
        setToolTip(QStringLiteral("在波形图中拖动矩形，为当前候选添加 AND 框。也可用下方数值输入；Escape 取消拖动。"));
        auto *cancel = new QAction(this); cancel->setShortcut(QKeySequence(Qt::Key_Escape)); cancel->setShortcutContext(Qt::WidgetShortcut); addAction(cancel);
        connect(cancel, &QAction::triggered, this, [this]() { cancelInteraction(); });
    }
    void cancelInteraction() { m_dragging = false; update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(m_owner->m_border); p.setBrush(m_owner->m_plotBg); p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6); p.setBrush(Qt::NoBrush);
        p.setPen(m_owner->m_text); p.drawText(QRect(12, 7, width() - 24, 20), QStringLiteral("捕获波形 · 拖动添加候选 %1 的框").arg(m_owner->m_unit->value()));
        m_area = QRectF(65, 36, std::max(1, width() - 88), std::max(1, height() - 84));
        m_x0 = -0.5; m_x1 = 1.5; m_y0 = -100; m_y1 = 100;
        if (m_owner->m_haveContext) {
            const auto &c = m_owner->m_context; const double step = 1000.0 * c.sampleStride / c.sourceSampleRate;
            m_x0 = -c.preSamples * step; m_x1 = c.postSamples * step;
            if (!(m_x1 > m_x0)) { m_x0 = -0.5; m_x1 = 1.5; }
        }
        for (const auto &r : m_owner->m_waveforms) for (float value : r.waveform) {
            if (!std::isfinite(value)) continue;
            m_y0 = std::min(m_y0, double(value) * 1.1e6); m_y1 = std::max(m_y1, double(value) * 1.1e6);
        }
        const int entry = m_owner->electrodeIndex();
        if (entry >= 0) for (const auto &candidate : m_owner->m_definitions[entry].candidates) for (const auto &box : candidate.boxes) {
            m_x0 = std::min(m_x0, box.timeMinMs); m_x1 = std::max(m_x1, box.timeMaxMs);
            m_y0 = std::min(m_y0, box.voltageMinUv * (box.voltageMinUv < 0 ? 1.1 : 0.9)); m_y1 = std::max(m_y1, box.voltageMaxUv * (box.voltageMaxUv > 0 ? 1.1 : 0.9));
        }
        p.setPen(m_owner->m_axis); p.drawRect(m_area);
        QFont font = p.font(); font.setPixelSize(11); p.setFont(font);
        for (int i = 0; i <= 2; ++i) {
            const double x = m_x0 + (m_x1 - m_x0) * i / 2;
            const double y = m_y0 + (m_y1 - m_y0) * i / 2;
            p.drawText(QRectF(point(x, 0).x() - 40, m_area.bottom() + 3, 80, 16), Qt::AlignCenter, n(x));
            p.drawText(QRectF(18, point(0, y).y() - 8, 42, 16), Qt::AlignRight | Qt::AlignVCenter, n(y));
        }
        p.drawText(QRectF(m_area.left(), height() - 22, m_area.width(), 18), Qt::AlignCenter, QStringLiteral("相对阈值时间 (ms)"));
        p.save(); p.translate(12, m_area.center().y()); p.rotate(-90); p.drawText(QRectF(-m_area.height() / 2, -9, m_area.height(), 18), Qt::AlignCenter, QStringLiteral("输入等效 µV")); p.restore();
        p.save(); p.setClipRect(m_area); p.setPen(QPen(m_owner->m_grid, 1, Qt::DashLine)); p.drawLine(point(0, m_y0), point(0, m_y1)); p.drawLine(point(m_x0, 0), point(m_x1, 0));
        for (const auto &r : m_owner->m_waveforms) {
            const auto &e = r.event; if (e.sourceSampleRate <= 0 || !std::isfinite(e.sourceSampleRate) || e.sampleStride < 1) continue;
            const double step = 1000.0 * e.sampleStride / e.sourceSampleRate; QPainterPath path; bool start = true;
            for (int i = 0; i < r.waveform.size(); ++i) {
                if (!std::isfinite(r.waveform[i])) { start = true; continue; }
                const QPointF pos = point((i - e.preSamples) * step, r.waveform[i] * 1e6);
                if (start) { path.moveTo(pos); start = false; } else path.lineTo(pos);
            }
            QColor wave = m_owner->m_wave; wave.setAlpha(100); p.setPen(QPen(wave, 1)); p.drawPath(path);
        }
        if (entry >= 0) for (const auto &candidate : m_owner->m_definitions[entry].candidates) {
            QColor color = QColor::fromHsv((candidate.unitId * 71 + 15) % 360, m_owner->m_plotBg.lightnessF() > 0.5 ? 200 : 160, m_owner->m_plotBg.lightnessF() > 0.5 ? 155 : 220);
            for (int i = 0; i < candidate.boxes.size(); ++i) {
                const auto &box = candidate.boxes[i]; const QRectF rect = QRectF(point(box.timeMinMs, box.voltageMaxUv), point(box.timeMaxMs, box.voltageMinUv)).normalized();
                QColor fill = color; fill.setAlpha(23); p.setBrush(fill); p.setPen(QPen(color, candidate.unitId == m_owner->m_unit->value() ? 2 : 1)); p.drawRect(rect);
                p.drawText(rect.adjusted(3, 2, -2, -2), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("%1.%2").arg(candidate.unitId).arg(i + 1));
            }
        }
        if (m_dragging) { QColor fill = m_owner->m_accent; fill.setAlpha(30); p.setBrush(fill); p.setPen(QPen(m_owner->m_accent, 1.5, Qt::DashLine)); p.drawRect(QRectF(m_start, m_end).normalized().intersected(m_area)); }
        p.restore();
        if (m_owner->m_waveforms.isEmpty()) { p.setPen(m_owner->m_secondary); p.drawText(m_area, Qt::AlignCenter | Qt::TextWordWrap, m_owner->m_haveContext ? QStringLiteral("当前捕获没有留存波形；可输入数值框，或再次捕获") : QStringLiteral("先捕获当前通道的检测上下文与波形")); }
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (!m_owner->m_haveContext || event->button() != Qt::LeftButton || !m_area.contains(event->position())) return;
        setFocus(); m_start = m_end = event->position(); m_dragging = true; update();
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (m_dragging) { m_end = event->position(); update(); } }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (!m_dragging || event->button() != Qt::LeftButton) return;
        m_end = event->position(); m_dragging = false;
        const QRectF r = QRectF(m_start, m_end).normalized().intersected(m_area);
        if (r.width() >= 3 && r.height() >= 3) {
            SpikeWaveformBox b; b.timeMinMs = x(r.left()); b.timeMaxMs = x(r.right()); b.voltageMinUv = y(r.bottom()); b.voltageMaxUv = y(r.top());
            m_owner->addBox(m_owner->m_unit->value(), b);
        }
        update();
    }
private:
    QPointF point(double x, double y) const { return {m_area.left() + (x - m_x0) / (m_x1 - m_x0) * m_area.width(), m_area.bottom() - (y - m_y0) / (m_y1 - m_y0) * m_area.height()}; }
    double x(double px) const { return m_x0 + (px - m_area.left()) / m_area.width() * (m_x1 - m_x0); }
    double y(double py) const { return m_y0 + (m_area.bottom() - py) / m_area.height() * (m_y1 - m_y0); }
    SpikeRuleEditor *m_owner;
    QRectF m_area;
    double m_x0 = -0.5, m_x1 = 1.5, m_y0 = -100, m_y1 = 100;
    bool m_dragging = false;
    QPointF m_start, m_end;
};

SpikeRuleEditor::SpikeRuleEditor(QWidget *parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("spikeRuleEditor")); setWindowTitle(QStringLiteral("未来事件 · 波形框规则")); setAttribute(Qt::WA_QuitOnClose, false);
    resize(1050, 800); setMinimumSize(760, 650);
    auto *root = new QVBoxLayout(this); root->setContentsMargins(12, 12, 12, 12); root->setSpacing(7);
    auto *heading = new QHBoxLayout; auto *title = new QLabel(QStringLiteral("波形框规则")); title->setObjectName(QStringLiteral("title")); heading->addWidget(title);
    auto *qualification = caption("ruleQualification"); qualification->setText(QStringLiteral("仅作用于启用后的新事件 · 候选形态分类，不是自动单神经元分离")); heading->addWidget(qualification, 1); root->addLayout(heading);
    auto *panel = new QWidget; panel->setProperty("role", "page-control-panel"); auto *row = new QHBoxLayout(panel); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(6);
    row->addWidget(new QLabel(QStringLiteral("通道"))); m_laneSpin = new QSpinBox; m_laneSpin->setObjectName(QStringLiteral("ruleLane")); m_laneSpin->setRange(0, 1023); row->addWidget(m_laneSpin);
    auto *capture = button(QStringLiteral("捕获当前波形"), "ruleCapture"); row->addWidget(capture);
    row->addWidget(new QLabel(QStringLiteral("候选"))); m_unit = new QSpinBox; m_unit->setObjectName(QStringLiteral("ruleUnit")); m_unit->setRange(1, 32); row->addWidget(m_unit); row->addStretch();
    auto *load = button(QStringLiteral("载入…"), "ruleLoad"); auto *save = button(QStringLiteral("保存草稿…"), "ruleSave"); row->addWidget(load); row->addWidget(save); root->addWidget(panel);
    m_contextLabel = caption("ruleContext"); root->addWidget(m_contextLabel);
    auto *splitter = new QSplitter(Qt::Horizontal); splitter->setChildrenCollapsible(false);
    m_plot = new SpikeRulePlot(this); splitter->addWidget(m_plot);
    auto *listPanel = new QWidget; listPanel->setProperty("role", "page-control-panel"); auto *listLayout = new QVBoxLayout(listPanel); listLayout->setContentsMargins(6, 0, 0, 0); listLayout->setSpacing(6);
    auto *listTitle = caption("ruleBoxCaption"); listTitle->setText(QStringLiteral("同一候选的所有框均须匹配（AND）\n多个候选同时匹配 → 歧义，不强行分配")); listLayout->addWidget(listTitle);
    m_boxes = new QListWidget; m_boxes->setObjectName(QStringLiteral("ruleBoxList")); m_boxes->setMinimumWidth(210); m_boxes->setWordWrap(true); m_boxes->setTextElideMode(Qt::ElideNone); m_boxes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); listLayout->addWidget(m_boxes, 1);
    auto *deleteRow = new QHBoxLayout; m_remove = button(QStringLiteral("删除所选框"), "ruleDeleteBox"); m_clear = button(QStringLiteral("清除此电极"), "ruleClearElectrode"); deleteRow->addWidget(m_remove); deleteRow->addWidget(m_clear); listLayout->addLayout(deleteRow);
    m_bind = button(QStringLiteral("将此电极规则绑定到捕获上下文"), "ruleBindContext"); listLayout->addWidget(m_bind);
    splitter->addWidget(listPanel); splitter->setSizes({680, 300}); root->addWidget(splitter, 1);
    auto *numericPanel = new QWidget; numericPanel->setProperty("role", "page-control-panel"); auto *numeric = new QGridLayout(numericPanel); numeric->setContentsMargins(0, 0, 0, 0); numeric->setSpacing(6);
    m_timeFrom = new QDoubleSpinBox; m_timeTo = new QDoubleSpinBox; m_voltageFrom = new QDoubleSpinBox; m_voltageTo = new QDoubleSpinBox;
    for (auto *v : {m_timeFrom, m_timeTo}) { v->setRange(-10000, 10000); v->setDecimals(5); v->setSingleStep(0.05); v->setSuffix(QStringLiteral(" ms")); }
    for (auto *v : {m_voltageFrom, m_voltageTo}) { v->setRange(-1e9, 1e9); v->setDecimals(3); v->setSingleStep(10); v->setSuffix(QStringLiteral(" µV")); }
    m_timeFrom->setObjectName(QStringLiteral("ruleTimeFrom")); m_timeTo->setObjectName(QStringLiteral("ruleTimeTo")); m_voltageFrom->setObjectName(QStringLiteral("ruleVoltageFrom")); m_voltageTo->setObjectName(QStringLiteral("ruleVoltageTo"));
    m_timeFrom->setValue(-0.05); m_timeTo->setValue(0.15); m_voltageFrom->setValue(-100); m_voltageTo->setValue(-20);
    numeric->addWidget(new QLabel(QStringLiteral("时间范围")), 0, 0); numeric->addWidget(m_timeFrom, 0, 1); numeric->addWidget(m_timeTo, 0, 2);
    numeric->addWidget(new QLabel(QStringLiteral("电压范围")), 1, 0); numeric->addWidget(m_voltageFrom, 1, 1); numeric->addWidget(m_voltageTo, 1, 2);
    m_add = button(QStringLiteral("添加 AND 框"), "ruleAddBox"); numeric->addWidget(m_add, 0, 3, 2, 1); numeric->setColumnStretch(1, 1); numeric->setColumnStretch(2, 1); root->addWidget(numericPanel);
    m_preview = caption("rulePreview"); root->addWidget(m_preview);
    auto *applyRow = new QHBoxLayout; m_revision = caption("ruleRevision"); applyRow->addWidget(m_revision, 1); m_apply = button(QStringLiteral("提交规则用于未来事件"), "ruleApply"); m_apply->setProperty("variant", "primary"); applyRow->addWidget(m_apply); root->addLayout(applyRow);
    auto *detailsButton = button(QStringLiteral("兼容性与边界详情 ▾"), "ruleDetailsToggle"); detailsButton->setCheckable(true); root->addWidget(detailsButton, 0, Qt::AlignLeft);
    m_details = caption("ruleDetails"); m_details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto *detailsViewport = new QScrollArea; detailsViewport->setObjectName(QStringLiteral("ruleDetailsViewport")); detailsViewport->setWidgetResizable(true); detailsViewport->setFrameShape(QFrame::NoFrame); detailsViewport->setMinimumHeight(80); detailsViewport->setMaximumHeight(120); detailsViewport->setWidget(m_details); root->addWidget(detailsViewport); detailsViewport->hide(); m_feedback = caption("ruleFeedback"); root->addWidget(m_feedback);
    connect(detailsButton, &QPushButton::toggled, this, [this, detailsViewport](bool visible) { detailsViewport->setVisible(visible); setMinimumHeight(visible ? 770 : 650); });
    connect(m_laneSpin, &QSpinBox::valueChanged, this, &SpikeRuleEditor::setLane);
    connect(capture, &QPushButton::clicked, this, [this]() { emit captureRequested(m_lane); });
    connect(m_unit, &QSpinBox::valueChanged, m_plot, qOverload<>(&QWidget::update));
    connect(m_boxes, &QListWidget::currentRowChanged, this, [this]() { m_remove->setEnabled(m_boxes->currentRow() >= 0); });
    connect(m_remove, &QPushButton::clicked, this, &SpikeRuleEditor::removeSelectedBox); connect(m_clear, &QPushButton::clicked, this, &SpikeRuleEditor::clearElectrode); connect(m_bind, &QPushButton::clicked, this, &SpikeRuleEditor::bindCapturedContext);
    auto *removeAction = new QAction(m_boxes); removeAction->setShortcut(QKeySequence(Qt::Key_Delete)); removeAction->setShortcutContext(Qt::WidgetShortcut); m_boxes->addAction(removeAction); connect(removeAction, &QAction::triggered, this, &SpikeRuleEditor::removeSelectedBox);
    connect(m_add, &QPushButton::clicked, this, [this]() { addBox(m_unit->value(), {m_timeFrom->value(), m_timeTo->value(), m_voltageFrom->value(), m_voltageTo->value()}); });
    connect(m_apply, &QPushButton::clicked, this, &SpikeRuleEditor::requestApply);
    connect(load, &QPushButton::clicked, this, [this]() { const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("载入波形框规则"), {}, QStringLiteral("规则 JSON (*.json);;所有文件 (*)")); if (!path.isEmpty()) loadRules(path); });
    connect(save, &QPushButton::clicked, this, [this]() { const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存规则草稿（服务应用时分配版本）"), {}, QStringLiteral("规则 JSON (*.json)")); if (!path.isEmpty()) saveRules(path); });
    refresh();
}

int SpikeRuleEditor::electrodeIndex() const {
    if (!m_haveContext) return -1;
    for (int i = 0; i < m_definitions.size(); ++i) if (m_definitions[i].context.electrode == m_context.electrode &&
        (m_definitions[i].context.tdmPhase >= 0) == (m_context.tdmPhase >= 0)) return i;
    return -1;
}
void SpikeRuleEditor::setLane(int lane) {
    lane = std::clamp(lane, 0, 1023); { const QSignalBlocker b(m_laneSpin); m_laneSpin->setValue(lane); }
    if (lane == m_lane) return;
    m_lane = lane; m_haveContext = false; m_waveforms.clear(); m_plot->cancelInteraction(); refresh(); emit captureRequested(lane);
}
void SpikeRuleEditor::setCapturedContext(const SpikeRuleContext &context, const QVector<SpikeArchiveRecord> &waveforms, const QString &status) {
    m_plot->cancelInteraction(); QString error; m_waveforms.clear();
    m_haveContext = SpikeRuleSet::validateContext(context, &error);
    if (!m_haveContext) { m_feedback->setText(QStringLiteral("无法捕获有效上下文：%1").arg(error)); refresh(); return; }
    m_context = context;
    // The preview is deliberately bounded and copied, never a pointer into the
    // processing ring. Capturing is read-only and does not subscribe to capture.
    for (int i = std::max(0, int(waveforms.size()) - 80); i < waveforms.size(); ++i)
        if (waveforms[i].waveform.size() <= SpikeRuleSet::kMaxSnippetSamples) m_waveforms.append(waveforms[i]);
    m_feedback->setText(status.isEmpty() ? QStringLiteral("已捕获上下文与最多80条波形；后续编辑不会改变历史事件") : status); refresh();
}
void SpikeRuleEditor::setAppliedRules(SpikeRuleSet::Snapshot rules, const QString &status) {
    m_applied = std::move(rules);
    const QVector<SpikeElectrodeRules> applied = m_applied ? m_applied->electrodes() : QVector<SpikeElectrodeRules>{};
    if (m_dirty && sameDefinitions(m_definitions, applied)) { m_definitions = applied; m_dirty = false; }
    else if (!m_dirty) m_definitions = m_requested ? m_requested->electrodes() : applied;
    if (!status.isEmpty()) m_feedback->setText(status); refresh();
}
void SpikeRuleEditor::setRequestedRules(SpikeRuleSet::Snapshot rules) {
    m_requested = std::move(rules);
    if (!m_dirty) m_definitions = m_requested ? m_requested->electrodes() : (m_applied ? m_applied->electrodes() : QVector<SpikeElectrodeRules>{});
    refresh();
}
void SpikeRuleEditor::setServiceStatus(const QString &status) { m_feedback->setText(status); }
void SpikeRuleEditor::markEdited() { m_dirty = true; m_plot->cancelInteraction(); refresh(); }
bool SpikeRuleEditor::addBox(int unitId, const SpikeWaveformBox &box) {
    if (!m_haveContext) { m_feedback->setText(QStringLiteral("先捕获当前通道上下文，再添加框")); return false; }
    QString error;
    if (!SpikeRuleSet::validateBox(box, &error)) { m_feedback->setText(QStringLiteral("框无效：%1").arg(error)); return false; }
    auto definitions = m_definitions; int index = electrodeIndex();
    if (index >= 0 && SpikeRuleSet::compatibilityFingerprint(definitions[index].context) != SpikeRuleSet::compatibilityFingerprint(m_context)) { m_feedback->setText(QStringLiteral("现有规则与捕获上下文不兼容；请明确绑定到新上下文，或清除此电极规则")); return false; }
    if (index < 0) { SpikeElectrodeRules entry; entry.context = m_context; definitions.append(entry); index = int(definitions.size()) - 1; }
    auto &candidates = definitions[index].candidates; int candidate = -1;
    for (int i = 0; i < candidates.size(); ++i) if (candidates[i].unitId == unitId) candidate = i;
    if (candidate < 0) { SpikeCandidateRule rule; rule.unitId = unitId; candidates.append(rule); candidate = int(candidates.size()) - 1; }
    candidates[candidate].boxes.append(box);
    if (!SpikeRuleSet::create(1, definitions, &error)) { m_feedback->setText(QStringLiteral("未添加：%1").arg(error)); return false; }
    m_definitions = std::move(definitions); markEdited(); m_feedback->setText(QStringLiteral("已添加候选 %1 的 AND 框；尚未提交到处理服务").arg(unitId)); return true;
}
void SpikeRuleEditor::removeSelectedBox() {
    const auto *item = m_boxes->currentItem(); const int index = electrodeIndex(); if (!item || index < 0) return;
    const int unitId = item->data(Qt::UserRole).toInt(), box = item->data(Qt::UserRole + 1).toInt();
    auto &candidates = m_definitions[index].candidates;
    for (int i = 0; i < candidates.size(); ++i) if (candidates[i].unitId == unitId && box >= 0 && box < candidates[i].boxes.size()) {
        candidates[i].boxes.removeAt(box); if (candidates[i].boxes.isEmpty()) candidates.removeAt(i); break;
    }
    if (candidates.isEmpty()) m_definitions.removeAt(index); markEdited(); m_feedback->setText(QStringLiteral("已从草稿删除框；已应用规则仍保持原版本，直至提交"));
}
void SpikeRuleEditor::clearElectrode() { const int i = electrodeIndex(); if (i < 0) return; m_definitions.removeAt(i); markEdited(); m_feedback->setText(QStringLiteral("此电极的规则草稿已清空；提交后仅影响未来事件")); }
void SpikeRuleEditor::bindCapturedContext() {
    const int i = electrodeIndex(); if (i < 0 || !m_haveContext) return;
    m_definitions[i].context = m_context; markEdited(); m_feedback->setText(QStringLiteral("已将此电极规则明确绑定到捕获上下文；请检查预览并提交"));
}
bool SpikeRuleEditor::loadRules(const QString &path) {
    QString error; const auto rules = SpikeRuleSet::load(path, &error);
    if (!rules) { m_feedback->setText(QStringLiteral("规则未载入：%1").arg(error)); return false; }
    m_definitions = rules->electrodes(); markEdited(); m_feedback->setText(QStringLiteral("已载入文件版本 %1 到草稿；尚未应用。服务提交时分配实际新版本").arg(rules->revision())); return true;
}
bool SpikeRuleEditor::saveRules(const QString &path) {
    QString error; const auto rules = SpikeRuleSet::create(1, m_definitions, &error);
    if (!rules || !rules->save(path, &error)) { m_feedback->setText(QStringLiteral("规则未保存：%1").arg(error)); return false; }
    m_feedback->setText(QStringLiteral("规则草稿已保存；文件版本1用于交换，不代表实际应用边界。历史事件的规则版本以归档为准")); return true;
}
void SpikeRuleEditor::requestApply() {
    QString error; if (!SpikeRuleSet::create(1, m_definitions, &error)) { m_feedback->setText(QStringLiteral("不能提交：%1").arg(error)); return; }
    m_feedback->setText(QStringLiteral("已请求应用；等待处理服务报告实际版本与生效边界")); emit applyRulesRequested(m_definitions);
}
void SpikeRuleEditor::refresh() {
    const int index = electrodeIndex(); const bool compatible = index < 0 || SpikeRuleSet::compatibilityFingerprint(m_definitions[index].context) == SpikeRuleSet::compatibilityFingerprint(m_context);
    m_contextLabel->setText(m_haveContext ? QStringLiteral("通道 %1 · ADC %2 / 物理电极 %3 / 相位 %4 · %5 Hz ÷ %6 · %7 条捕获波形 · %8").arg(m_lane).arg(m_context.adcChannel).arg(m_context.electrode).arg(m_context.tdmPhase).arg(n(m_context.sourceSampleRate)).arg(m_context.sampleStride).arg(m_waveforms.size()).arg(index < 0 ? QStringLiteral("此物理电极 / 模式尚无规则") : compatible ? QStringLiteral("上下文兼容") : QStringLiteral("规则上下文不兼容；未来事件会标记为不兼容")) : QStringLiteral("尚未捕获有效检测上下文；打开本窗口不会启动采集"));
    const int selected = m_boxes->currentRow(); m_boxes->clear();
    if (index >= 0) for (const auto &candidate : m_definitions[index].candidates) for (int b = 0; b < candidate.boxes.size(); ++b) {
        const auto &box = candidate.boxes[b];
        auto *item = new QListWidgetItem(QStringLiteral("候选 %1 · 框 %2\n%3…%4 ms  /  %5…%6 µV").arg(candidate.unitId).arg(b + 1).arg(n(box.timeMinMs), n(box.timeMaxMs), n(box.voltageMinUv), n(box.voltageMaxUv)), m_boxes);
        item->setSizeHint(QSize(0, 72)); item->setToolTip(item->text()); item->setData(Qt::UserRole, candidate.unitId); item->setData(Qt::UserRole + 1, b);
    }
    if (selected >= 0 && selected < m_boxes->count()) m_boxes->setCurrentRow(selected);
    m_add->setEnabled(m_haveContext && compatible); m_remove->setEnabled(m_boxes->currentRow() >= 0); m_clear->setEnabled(index >= 0); m_bind->setEnabled(m_haveContext && index >= 0 && !compatible);
    m_apply->setEnabled(m_dirty);
    QString error; const auto draft = SpikeRuleSet::create(1, m_definitions, &error); int assigned = 0, ambiguous = 0, incompatible = 0, unassigned = 0;
    if (draft && m_haveContext) for (const auto &r : m_waveforms) {
        const auto result = draft->classify(r.event, r.waveform.constData(), int(r.waveform.size()), m_context);
        if (result.status == SpikeClassificationStatus::Assigned) ++assigned;
        else if (result.status == SpikeClassificationStatus::Ambiguous) ++ambiguous;
        else if (result.status == SpikeClassificationStatus::Incompatible) ++incompatible;
        else ++unassigned;
    }
    m_preview->setText(QStringLiteral("仅捕获波形的草稿预览：匹配 %1 · 歧义 %2 · 不兼容 %3 · 未分配 %4 · 不会回写已有事件").arg(assigned).arg(ambiguous).arg(incompatible).arg(unassigned));
    const QString requested = m_requested && (!m_applied || m_requested->revision() != m_applied->revision())
        ? QStringLiteral(" · 会话已保存 / 待应用版本 %1").arg(m_requested->revision()) : QString();
    m_revision->setText(QStringLiteral("服务实际应用版本：%1%2 · %3 · 草稿含 %4 个电极").arg(m_applied ? QString::number(m_applied->revision()) : QStringLiteral("0（无当前确认）"), requested, m_dirty ? QStringLiteral("有未应用编辑") : QStringLiteral("无本地未提交编辑")).arg(m_definitions.size()));
    m_details->setText(QStringLiteral("所有框坐标使用阈值交越对齐的毫秒和输入等效 µV；边界包含在内。每个候选的框为 AND，跨候选匹配为歧义，绝不取第一个命中。\n兼容性严格核对物理电极、ADC/TDM、采样率、增益、参考、滤波、阈值、不应期和片段长度。捕获后更改这些设置将使旧上下文规则不兼容，需明确重新绑定。\n新规则仅由处理服务在新交越的版本边界生效；已挂起的波形仍保留原版本。人工留存标签与持久归档注释独立。\n捕获上下文指纹：%1\n服务版本用于归档溯源；草稿文件的版本号不声明实际应用边界。%2").arg(m_haveContext ? QString::fromLatin1(SpikeRuleSet::compatibilityFingerprint(m_context).toHex()) : QStringLiteral("无"), error.isEmpty() ? QString() : QStringLiteral("\n验证：") + error));
    m_plot->update();
}
void SpikeRuleEditor::setWaveTheme(const QMap<QString, QString> &palette) {
    const auto pick = [&](const char *key, QColor &c) { const QColor value(palette.value(QLatin1String(key))); if (value.isValid()) c = value; };
    pick("appBg", m_windowBg); pick("plotBg", m_plotBg); pick("border", m_border); pick("text", m_text); pick("title", m_secondary); pick("axis", m_axis); pick("wave", m_wave); pick("accent", m_accent); pick("grid", m_grid); update(); m_plot->update();
}
void SpikeRuleEditor::paintEvent(QPaintEvent *) { QPainter p(this); p.fillRect(rect(), m_windowBg); }
void SpikeRuleEditor::hideEvent(QHideEvent *event) { m_plot->cancelInteraction(); QWidget::hideEvent(event); }
} // namespace ccv2
