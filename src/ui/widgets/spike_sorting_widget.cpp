#include "ui/widgets/spike_sorting_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ccv2 {
namespace {
QString featureName(SpikeFeatureAxis axis) {
    switch (axis) {
    case SpikeFeatureAxis::PeakToPeak: return QStringLiteral("峰峰值 (µV)");
    case SpikeFeatureAxis::SignedPeak: return QStringLiteral("带符号峰值 (µV)");
    case SpikeFeatureAxis::Energy: return QStringLiteral("能量 (µV²·ms)");
    }
    return {};
}
QString number(double value) {
    return std::isfinite(value) ? QString::number(value, 'g', 5) : QStringLiteral("n/a");
}
void padRange(double &lo, double &hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi)) { lo = -1; hi = 1; return; }
    const double pad = hi > lo ? (hi - lo) * 0.06 : std::max(1.0, std::abs(lo) * 0.06);
    lo -= pad; hi += pad;
}
} // namespace

class SpikeSortingPlot : public QWidget {
public:
    enum Kind { Features, Waveforms, Raster, Isi };
    SpikeSortingPlot(SpikeSortingWidget *owner, Kind kind)
        : QWidget(owner), m_owner(owner), m_kind(kind) {
        setMinimumSize(280, 190);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (kind == Features) {
            setObjectName(QStringLiteral("candidateFeaturePlot"));
            setToolTip(QStringLiteral("Drag a rectangle to select events; Shift adds. Click a point for one event. Selection freezes this view, not acquisition."));
        }
    }
    void cancelInteraction() {
        m_dragging = false;
        m_points.clear();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(m_owner->m_plotBg);
        p.setPen(m_owner->m_border);
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);
        p.setBrush(Qt::NoBrush);
        m_area = QRectF(64, 31, std::max(1, width() - 84), std::max(1, height() - 78));
        m_points.clear();
        const auto rows = m_owner->m_model.rowsForUnit(m_owner->unitFilter());
        switch (m_kind) {
        case Features: drawFeatures(p, rows); break;
        case Waveforms: drawWaveforms(p, rows); break;
        case Raster: drawRaster(p, rows); break;
        case Isi: drawIsi(p); break;
        }
        if (m_dragging) {
            p.setPen(QPen(m_owner->m_accent, 1.5, Qt::DashLine));
            QColor fill = m_owner->m_accent; fill.setAlpha(30); p.setBrush(fill);
            p.drawRect(QRectF(m_start, m_end).normalized().intersected(m_area));
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (m_kind != Features || event->button() != Qt::LeftButton ||
            !m_area.contains(event->position())) return;
        m_owner->m_freeze->setChecked(true);
        m_start = m_end = event->position();
        m_dragging = true;
        update();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!m_dragging) return;
        m_end = event->position(); update();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (!m_dragging || event->button() != Qt::LeftButton) return;
        m_end = event->position(); m_dragging = false;
        QVector<int> selected;
        const QRectF box = QRectF(m_start, m_end).normalized();
        if (box.width() + box.height() < 8) {
            double best = 81; int row = -1;
            for (const auto &item : m_points) {
                const QPointF delta = item.second - m_end;
                const double d = delta.x() * delta.x() + delta.y() * delta.y();
                if (d < best) { best = d; row = item.first; }
            }
            if (row >= 0) selected.append(row);
        } else {
            for (const auto &item : m_points) if (box.contains(item.second)) selected.append(item.first);
        }
        m_owner->selectRows(selected, event->modifiers().testFlag(Qt::ShiftModifier));
        update();
    }

private:
    QColor unitColor(int unit) const {
        if (unit < 0) return m_owner->m_wave;
        const bool light = m_owner->m_plotBg.lightnessF() > 0.5;
        return QColor::fromHsv((unit * 71 + 15) % 360, light ? 200 : 160, light ? 155 : 220);
    }
    QPointF point(double x, double y) const {
        return {m_area.left() + (x - m_x0) / (m_x1 - m_x0) * m_area.width(),
                m_area.bottom() - (y - m_y0) / (m_y1 - m_y0) * m_area.height()};
    }
    void axes(QPainter &p, const QString &title, const QString &x, const QString &y, bool integerY = false) {
        p.setPen(m_owner->m_text);
        p.drawText(QRectF(10, 5, width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, title);
        p.setPen(m_owner->m_axis);
        p.drawRect(m_area);
        QFont font = p.font(); font.setPixelSize(11); p.setFont(font);
        for (int i = 0; i <= 2; ++i) {
            const double t = i / 2.0;
            const double px = m_area.left() + m_area.width() * t;
            p.drawText(QRectF(px - 35, m_area.bottom() + 3, 70, 16), Qt::AlignCenter, number(m_x0 + (m_x1 - m_x0) * t));
            const double lo = std::max(0.0, std::ceil(m_y0)), hi = std::floor(m_y1);
            const double yv = integerY ? std::floor(lo + (hi - lo) * t) : m_y0 + (m_y1 - m_y0) * t;
            if (integerY && i == 1 && (yv == lo || yv == hi)) continue;
            const double py = point(m_x0, yv).y();
            p.drawText(QRectF(0, py - 8, m_area.left() - 5, 16), Qt::AlignRight | Qt::AlignVCenter, number(yv));
        }
        p.drawText(QRectF(m_area.left(), height() - 23, m_area.width(), 18), Qt::AlignCenter, x);
        p.save();
        p.translate(12, m_area.center().y()); p.rotate(-90);
        p.drawText(QRectF(-m_area.height() / 2, -10, m_area.height(), 18), Qt::AlignCenter, y);
        p.restore();
    }
    void empty(QPainter &p, const QString &text) {
        p.setPen(m_owner->m_secondary); p.drawText(m_area, Qt::AlignCenter | Qt::TextWordWrap, text);
    }
    void drawFeatures(QPainter &p, const QVector<int> &rows) {
        m_x0 = m_y0 = std::numeric_limits<double>::infinity();
        m_x1 = m_y1 = -m_x0;
        for (int row : rows) {
            const auto &f = m_owner->m_model.features()[row];
            const double x = f.value(m_owner->xAxis()), y = f.value(m_owner->yAxis());
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            m_x0 = std::min(m_x0, x); m_x1 = std::max(m_x1, x);
            m_y0 = std::min(m_y0, y); m_y1 = std::max(m_y1, y);
        }
        padRange(m_x0, m_x1); padRange(m_y0, m_y1);
        axes(p, QStringLiteral("特征分布 · 拖动框选"), featureName(m_owner->xAxis()), featureName(m_owner->yAxis()));
        p.save(); p.setClipRect(m_area);
        for (int row : rows) {
            const auto &f = m_owner->m_model.features()[row];
            const double x = f.value(m_owner->xAxis()), y = f.value(m_owner->yAxis());
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            const auto &e = m_owner->m_model.events()[row];
            const QPointF pos = point(x, y); m_points.append({row, pos});
            const bool selected = m_owner->m_selected.contains(e.sequence);
            p.setPen(selected ? QPen(m_owner->m_accent, 1.5) : QPen(Qt::NoPen));
            p.setBrush(unitColor(e.unitId)); p.drawEllipse(pos, selected ? 4 : 2.7, selected ? 4 : 2.7);
        }
        p.restore();
        if (m_points.isEmpty()) empty(p, QStringLiteral("当前留存窗口暂无有效特征"));
    }
    void drawWaveforms(QPainter &p, const QVector<int> &rows) {
        const auto &qc = m_owner->m_qc;
        const int length = m_owner->m_model.snippetLength();
        m_x0 = -qc.preSamples * qc.waveformStepMs;
        m_x1 = (length - 1 - qc.preSamples) * qc.waveformStepMs;
        if (m_x1 <= m_x0) { m_x0 = -1; m_x1 = 1; }
        double amplitude = 1;
        QVector<int> overlay;
        for (int row : rows) {
            const auto &e = m_owner->m_model.events()[row];
            if (!m_owner->m_model.features()[row].finiteWaveform ||
                !std::isfinite(e.sourceSampleRate) || e.sourceSampleRate <= 0 || e.sampleStride <= 0 ||
                e.preSamples < 0 || e.preSamples >= length) continue;
            if (!m_owner->m_selected.isEmpty() && !m_owner->m_selected.contains(e.sequence)) continue;
            overlay.append(row);
        }
        // Prefer the newest selected/visible waveforms; never paint thousands.
        if (overlay.size() > 80) overlay = overlay.mid(overlay.size() - 80);
        for (int row : overlay) {
            const auto &e = m_owner->m_model.events()[row];
            const double step = 1000.0 * e.sampleStride / e.sourceSampleRate;
            m_x0 = std::min(m_x0, -e.preSamples * step);
            m_x1 = std::max(m_x1, (length - 1 - e.preSamples) * step);
            const float *wave = m_owner->m_model.waveform(row);
            for (int j = 0; j < length; ++j) amplitude = std::max(amplitude, std::abs(wave[j] * 1e6));
        }
        for (int j = 0; j < qc.meanUv.size(); ++j)
            amplitude = std::max(amplitude, std::abs(qc.meanUv[j]) + qc.stddevUv[j]);
        m_y0 = -1.1 * amplitude; m_y1 = 1.1 * amplitude;
        axes(p, QStringLiteral("波形与均值 ±SD · n=%1 · 最多叠加80条").arg(qc.templateEvents),
             QStringLiteral("相对阈值时间 (ms)"), QStringLiteral("输入等效 µV"));
        p.save(); p.setClipRect(m_area);
        p.setPen(QPen(m_owner->m_grid, 1, Qt::DashLine));
        p.drawLine(point(0, m_y0), point(0, m_y1));
        for (int row : overlay) {
            const auto &e = m_owner->m_model.events()[row];
            const double step = 1000.0 * e.sampleStride / e.sourceSampleRate;
            QPainterPath path;
            for (int j = 0; j < length; ++j) {
                const auto pos = point((j - e.preSamples) * step, m_owner->m_model.waveform(row)[j] * 1e6);
                if (j == 0) path.moveTo(pos); else path.lineTo(pos);
            }
            QColor color = unitColor(e.unitId); color.setAlpha(90);
            p.setPen(QPen(color, 1)); p.drawPath(path);
        }
        if (!qc.meanUv.isEmpty()) {
            QPainterPath band, mean;
            for (int j = 0; j < length; ++j) {
                const double x = (j - qc.preSamples) * qc.waveformStepMs;
                const auto upper = point(x, qc.meanUv[j] + qc.stddevUv[j]);
                const auto mid = point(x, qc.meanUv[j]);
                if (j == 0) { band.moveTo(upper); mean.moveTo(mid); }
                else { band.lineTo(upper); mean.lineTo(mid); }
            }
            for (int j = length - 1; j >= 0; --j)
                band.lineTo(point((j - qc.preSamples) * qc.waveformStepMs, qc.meanUv[j] - qc.stddevUv[j]));
            band.closeSubpath(); p.fillPath(band, QColor(m_owner->m_wave.red(), m_owner->m_wave.green(), m_owner->m_wave.blue(), 35));
            p.setPen(QPen(m_owner->m_text, 2)); p.drawPath(mean);
        }
        p.restore();
        if (qc.templateEvents == 0) empty(p, QStringLiteral("暂无可用的同类波形模板"));
    }
    void drawRaster(QPainter &p, const QVector<int> &rows) {
        m_x0 = std::numeric_limits<double>::infinity(); m_x1 = -m_x0;
        m_y0 = -0.5; m_y1 = 1.5;
        int timed = 0;
        for (int row : rows) {
            const auto &e = m_owner->m_model.events()[row];
            if (!e.hasSourceTime() || !std::isfinite(e.sourceSampleRate)) continue;
            const double t = e.sourceTimeSeconds(); ++timed;
            m_x0 = std::min(m_x0, t); m_x1 = std::max(m_x1, t);
            m_y1 = std::max(m_y1, e.unitId + 0.5);
        }
        padRange(m_x0, m_x1);
        axes(p, QStringLiteral("事件时序 · 0 = 未分配"),
             QStringLiteral("源时间 (s)"), QStringLiteral("候选"), true);
        p.save(); p.setClipRect(m_area);
        for (int row : rows) {
            const auto &e = m_owner->m_model.events()[row];
            if (!e.hasSourceTime() || !std::isfinite(e.sourceSampleRate)) continue;
            const QPointF pos = point(e.sourceTimeSeconds(), std::max(0, e.unitId));
            p.setPen(QPen(unitColor(e.unitId), 1.5)); p.drawLine(pos + QPointF(0, -4), pos + QPointF(0, 4));
        }
        p.restore();
        if (timed == 0) empty(p, QStringLiteral("暂无可用的源时间戳"));
    }
    void drawIsi(QPainter &p) {
        const auto &qc = m_owner->m_qc;
        constexpr int bins = 50;
        int counts[bins] = {}; int overflow = 0; int maximum = 1;
        for (double ms : qc.intervalsMs) {
            if (ms >= 100) { ++overflow; continue; }
            const int bin = std::clamp(int(ms / 2), 0, bins - 1);
            maximum = std::max(maximum, ++counts[bin]);
        }
        m_x0 = 0; m_x1 = 100; m_y0 = 0; m_y1 = maximum * 1.1;
        axes(p, QStringLiteral("ISI 间隔 · 2 ms 分箱 · ≥100 ms: %1").arg(overflow),
             QStringLiteral("连续片段内间隔 (ms)"), QStringLiteral("计数"), true);
        p.save(); p.setClipRect(m_area);
        for (int i = 0; i < bins; ++i) {
            QRectF bar(point(i * 2, counts[i]), point((i + 1) * 2, 0));
            p.fillRect(bar.normalized().adjusted(0.5, 0, -0.5, 0), i == 0 ? m_owner->m_warning : m_owner->m_wave);
        }
        p.restore();
        if (qc.intervalsMs.isEmpty()) empty(p, QStringLiteral("需同一连续片段内至少2个有效事件"));
    }

    SpikeSortingWidget *m_owner;
    Kind m_kind;
    QRectF m_area;
    double m_x0 = -1, m_x1 = 1, m_y0 = -1, m_y1 = 1;
    QVector<QPair<int, QPointF>> m_points;
    bool m_dragging = false;
    QPointF m_start, m_end;
};

SpikeSortingWidget::SpikeSortingWidget(SpikeSnippetStore *store, QWidget *parent)
    : QWidget(parent, Qt::Window), m_store(store) {
    setObjectName(QStringLiteral("spikeSortingWorkspace"));
    setWindowTitle(QStringLiteral("候选单元 · Spike 分选"));
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(1120, 850);
    setMinimumSize(900, 680);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("候选单元"));
    title->setObjectName(QStringLiteral("title"));
    header->addWidget(title);
    auto *notice = new QLabel(QStringLiteral("人工候选标签 · 仅留存窗口 · 非单神经元分离结论"));
    notice->setProperty("role", "caption");
    notice->setWordWrap(true);
    notice->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(notice, 1);
    root->addLayout(header);

    auto *controlPanel = new QWidget;
    controlPanel->setProperty("role", "page-control-panel");
    auto *controlRows = new QVBoxLayout(controlPanel);
    controlRows->setContentsMargins(0, 0, 0, 0);
    controlRows->setSpacing(6);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    controls->addWidget(new QLabel(QStringLiteral("通道")));
    m_laneSpin = new QSpinBox; m_laneSpin->setObjectName(QStringLiteral("candidateLane"));
    m_laneSpin->setToolTip(QStringLiteral("通道编号从0开始；TDM时按当前相位对映射电极"));
    m_laneSpin->setRange(0, std::max(0, store ? store->channels() - 1 : 0)); controls->addWidget(m_laneSpin);
    controls->addWidget(new QLabel(QStringLiteral("显示")));
    m_unitFilter = new QComboBox; m_unitFilter->setObjectName(QStringLiteral("candidateFilter"));
    m_unitFilter->addItem(QStringLiteral("全部事件"), SpikeSortingModel::kAllUnits);
    m_unitFilter->addItem(QStringLiteral("未分配"), -1);
    for (int i = 1; i <= 32; ++i) m_unitFilter->addItem(QStringLiteral("候选 %1").arg(i), i);
    controls->addWidget(m_unitFilter);
    m_freeze = new QCheckBox(QStringLiteral("冻结当前快照"));
    m_freeze->setObjectName(QStringLiteral("candidateFreeze"));
    m_freeze->setToolTip(QStringLiteral("仅冻结此窗口；采集与分析继续。留存容量满时，选中的旧事件仍可能被覆盖。跳转或新时间线会清除旧选择并刷新快照"));
    controls->addWidget(m_freeze);
    auto *refresh = new QPushButton(QStringLiteral("刷新快照"));
    refresh->setProperty("variant", "secondary");
    refresh->setObjectName(QStringLiteral("candidateRefresh")); controls->addWidget(refresh);
    controls->addStretch(); controlRows->addLayout(controls);
    auto *featureControls = new QHBoxLayout;
    featureControls->setSpacing(8);
    featureControls->addWidget(new QLabel(QStringLiteral("特征 X"))); m_xAxis = new QComboBox;
    featureControls->addWidget(m_xAxis); featureControls->addWidget(new QLabel(QStringLiteral("Y"))); m_yAxis = new QComboBox;
    for (auto axis : {SpikeFeatureAxis::PeakToPeak, SpikeFeatureAxis::SignedPeak, SpikeFeatureAxis::Energy}) {
        m_xAxis->addItem(featureName(axis), int(axis)); m_yAxis->addItem(featureName(axis), int(axis));
    }
    m_yAxis->setCurrentIndex(1); featureControls->addWidget(m_yAxis);
    auto *selectAll = new QPushButton(QStringLiteral("选择全部可见")); selectAll->setObjectName(QStringLiteral("candidateSelectAll"));
    auto *clear = new QPushButton(QStringLiteral("清除选择"));
    clear->setObjectName(QStringLiteral("candidateClearSelection"));
    selectAll->setProperty("variant", "secondary"); clear->setProperty("variant", "secondary");
    featureControls->addWidget(selectAll); featureControls->addWidget(clear); featureControls->addStretch();
    controlRows->addLayout(featureControls);
    root->addWidget(controlPanel);
    auto *grid = new QGridLayout;
    grid->setSpacing(8);
    for (int i = 0; i < 4; ++i) {
        auto *plot = new SpikeSortingPlot(this, static_cast<SpikeSortingPlot::Kind>(i));
        m_plots.append(plot); grid->addWidget(plot, i / 2, i % 2);
    }
    root->addLayout(grid, 1);
    auto *labelPanel = new QWidget;
    labelPanel->setProperty("role", "page-control-panel");
    auto *labels = new QHBoxLayout(labelPanel);
    labels->setContentsMargins(0, 0, 0, 0);
    labels->setSpacing(8);
    m_selection = new QLabel; m_selection->setProperty("role", "data-value"); labels->addWidget(m_selection); labels->addStretch();
    m_unitSpin = new QSpinBox; m_unitSpin->setObjectName(QStringLiteral("candidateUnit")); m_unitSpin->setRange(1, 32);
    m_unitSpin->setPrefix(QStringLiteral("候选 ")); labels->addWidget(m_unitSpin);
    m_assign = new QPushButton(QStringLiteral("标记所选")); m_assign->setObjectName(QStringLiteral("candidateAssign")); m_assign->setProperty("variant", "primary"); labels->addWidget(m_assign);
    m_unassign = new QPushButton(QStringLiteral("移除标记")); m_unassign->setObjectName(QStringLiteral("candidateUnassign")); m_unassign->setProperty("variant", "secondary"); labels->addWidget(m_unassign);
    root->addWidget(labelPanel);
    m_coverage = new QLabel; m_coverage->setWordWrap(true); m_coverage->setProperty("role", "caption"); m_coverage->setObjectName(QStringLiteral("candidateCoverage")); root->addWidget(m_coverage);
    auto *qualityPanel = new QWidget;
    qualityPanel->setProperty("role", "page-control-panel");
    auto *qualityRow = new QHBoxLayout(qualityPanel);
    qualityRow->setContentsMargins(0, 0, 0, 0);
    m_qualitySummary = new QLabel; m_qualitySummary->setProperty("role", "status-line"); m_qualitySummary->setObjectName(QStringLiteral("candidateQualitySummary")); m_qualitySummary->setWordWrap(true);
    qualityRow->addWidget(m_qualitySummary, 1);
    auto *detailsButton = new QPushButton(QStringLiteral("质量详情 ▾"));
    detailsButton->setObjectName(QStringLiteral("candidateQualityDetails"));
    detailsButton->setProperty("variant", "secondary"); detailsButton->setCheckable(true);
    qualityRow->addWidget(detailsButton);
    root->addWidget(qualityPanel);
    auto *details = new QWidget;
    details->setProperty("role", "plot-area");
    details->setObjectName(QStringLiteral("candidateQualityDetailsPanel"));
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(10, 8, 10, 8);
    m_quality = new QLabel; m_quality->setWordWrap(true); m_quality->setProperty("role", "caption"); detailLayout->addWidget(m_quality);
    auto *caution = new QLabel(QStringLiteral("仅统计留存事件；ISI不跨越已知缺口。不应期、漏检和留存筛选都会影响指标；短ISI少不能证明单神经元分离。标签会随旧事件被覆盖或分析重置而失效，请及时导出。"));
    caution->setWordWrap(true); caution->setProperty("role", "caption"); detailLayout->addWidget(caution);
    root->addWidget(details); details->hide();
    connect(detailsButton, &QPushButton::toggled, this, [details, detailsButton](bool expanded) {
        details->setVisible(expanded);
        detailsButton->setText(expanded ? QStringLiteral("收起详情 ▴") : QStringLiteral("质量详情 ▾"));
    });
    m_feedback = new QLabel(QStringLiteral("拖动特征图框选，Shift追加 · 波形显示均值±SD · 标签仅随留存事件保存，请及时导出"));
    m_feedback->setWordWrap(true); m_feedback->setProperty("role", "caption"); m_feedback->setObjectName(QStringLiteral("candidateFeedback")); root->addWidget(m_feedback);

    connect(m_laneSpin, &QSpinBox::valueChanged, this, &SpikeSortingWidget::setLane);
    connect(refresh, &QPushButton::clicked, this, &SpikeSortingWidget::refreshData);
    connect(m_freeze, &QCheckBox::toggled, this, [this](bool frozen) { if (!frozen) refreshData(); });
    connect(m_unitFilter, &QComboBox::currentIndexChanged, this, [this]() {
        m_selected.clear(); refreshSummary(); updatePlots();
    });
    connect(m_xAxis, &QComboBox::currentIndexChanged, this, &SpikeSortingWidget::updatePlots);
    connect(m_yAxis, &QComboBox::currentIndexChanged, this, &SpikeSortingWidget::updatePlots);
    connect(selectAll, &QPushButton::clicked, this, [this]() { m_freeze->setChecked(true); selectRows(m_model.rowsForUnit(unitFilter()), false); });
    connect(clear, &QPushButton::clicked, this, [this]() { selectRows({}, false); });
    connect(m_assign, &QPushButton::clicked, this, [this]() { assignSelected(m_unitSpin->value()); });
    connect(m_unassign, &QPushButton::clicked, this, [this]() { assignSelected(-1); });
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &SpikeSortingWidget::refreshIfLive);
    refreshData();
}

int SpikeSortingWidget::unitFilter() const { return m_unitFilter->currentData().toInt(); }
SpikeFeatureAxis SpikeSortingWidget::xAxis() const { return static_cast<SpikeFeatureAxis>(m_xAxis->currentData().toInt()); }
SpikeFeatureAxis SpikeSortingWidget::yAxis() const { return static_cast<SpikeFeatureAxis>(m_yAxis->currentData().toInt()); }

void SpikeSortingWidget::setLane(int lane) {
    const int channels = m_store ? m_store->channels() : 0;
    lane = std::clamp(lane, 0, std::max(0, channels - 1));
    const QSignalBlocker blocker(m_laneSpin);
    m_laneSpin->setRange(0, std::max(0, channels - 1)); m_laneSpin->setValue(lane);
    if (m_lane != lane) { m_selected.clear(); m_feedback->clear(); }
    m_lane = lane; refreshData();
}

void SpikeSortingWidget::refreshData() {
    // An epoch/config refresh can arrive between mouse press and release.
    // Never apply screen coordinates from an old snapshot to replacement rows.
    for (auto *plot : m_plots) plot->cancelInteraction();
    if (!m_store) { m_model.clear(); refreshSummary(); updatePlots(); return; }
    const int channels = m_store->channels();
    {
        const QSignalBlocker blocker(m_laneSpin);
        m_laneSpin->setRange(0, std::max(0, channels - 1));
        m_lane = std::clamp(m_lane, 0, std::max(0, channels - 1));
        m_laneSpin->setValue(m_lane);
    }
    auto snapshot = m_store->snapshotLane(m_lane);
    if (snapshot.quality.epoch != m_snapshot.quality.epoch || snapshot.lane != m_snapshot.lane) {
        m_selected.clear();
        if (m_snapshot.lane >= 0)
            m_feedback->setText(QStringLiteral("时间线或通道已变更，已清除旧选择"));
    }
    if (!m_model.setSnapshot(m_lane, snapshot.events, snapshot.waveforms, snapshot.snippetLength)) {
        m_selected.clear();
        m_feedback->setText(QStringLiteral("快照暂不可用；请在分析配置完成后刷新"));
    }
    QSet<quint64> retained;
    for (int row : m_model.rowsForUnit(unitFilter())) retained.insert(m_model.events()[row].sequence);
    m_selected.intersect(retained);
    // The model owns the only retained waveform copy. Keep just summary facts.
    snapshot.events.clear(); snapshot.events.squeeze(); snapshot.waveforms.clear(); snapshot.waveforms.squeeze();
    m_snapshot = std::move(snapshot);
    refreshSummary(); updatePlots();
}

void SpikeSortingWidget::refreshIfLive() {
    if (!isVisible()) return;
    if (!m_freeze->isChecked()) { refreshData(); return; }
    if (!m_store) return;
    SpikeAnalysisQuality quality;
    m_store->snapshotAnalysis(nullptr, &quality);
    if (quality.epoch != m_snapshot.quality.epoch) {
        m_selected.clear(); refreshData();
        m_feedback->setText(QStringLiteral("时间线已变更，已清除旧选择并更新快照"));
    }
}

void SpikeSortingWidget::refreshSummary() {
    m_qc = m_model.unitQc(unitFilter());
    m_selection->setText(QStringLiteral("已选 %1 / 可见 %2").arg(m_selected.size()).arg(m_qc.retainedEvents));
    m_assign->setEnabled(!m_selected.isEmpty()); m_unassign->setEnabled(!m_selected.isEmpty());
    const int retained = int(m_model.events().size());
    const qint64 omitted = std::max<qint64>(0, m_snapshot.totalDetected - retained);
    QString identity;
    if (retained) {
        const auto &e = m_model.events().first();
        identity = e.tdmPhase < 0 ? QStringLiteral(" · ADC %1").arg(e.adcChannel)
            : QStringLiteral(" · ADC %1 / 电极 %2 / 相位 %3").arg(e.adcChannel).arg(e.electrode).arg(e.tdmPhase);
    }
    m_coverage->setText(QStringLiteral("epoch %1%2 · 留存 %3 / 检出 %4（上限 %5；窗口外 %6）· 已分析 %7 样本 · 噪声 %8 µV")
        .arg(m_snapshot.quality.epoch).arg(identity).arg(retained).arg(m_snapshot.totalDetected)
        .arg(m_snapshot.capacity).arg(omitted).arg(m_snapshot.observedSamples).arg(number(m_snapshot.noiseRmsV * 1e6)));
    const auto &quality = m_snapshot.quality;
    const QString coverage = quality.incomplete()
        ? QStringLiteral("覆盖详情：中断 %1，缺失 %2 帧，队列丢失 %3，无效 %4，未验证 %5，待完成窗口 %6，边界排除 %7%8")
              .arg(quality.discontinuities).arg(quality.missingSourceFrames).arg(quality.queueDroppedFrames)
              .arg(quality.invalidFrames).arg(quality.unverifiedFrames).arg(quality.pendingWindowEvents)
              .arg(quality.boundaryExcludedEvents).arg(quality.stoppedEarly ? QStringLiteral("；停止尾部未保证完整") : QString())
        : QStringLiteral("此分析区间未发现已知缺口；仅覆盖留存窗口，并非完整事件录制");
    const QString timing = m_qc.firstSourceSeconds >= 0
        ? QStringLiteral("%1–%2 s").arg(number(m_qc.firstSourceSeconds), number(m_qc.lastSourceSeconds)) : QStringLiteral("n/a");
    m_quality->setText(QStringLiteral("%1\n当前筛选 QC：源时间 %2 · ISI %3 · <2 ms %4 · 无效波形 %5 · 未知时间 %6 · 不兼容模板 %7")
        .arg(coverage, timing).arg(m_qc.intervalsMs.size()).arg(m_qc.shortIntervals)
        .arg(m_qc.invalidWaveforms).arg(m_qc.unknownTimes).arg(m_qc.incompatibleTemplates));
    QStringList alerts;
    if (quality.missingSourceFrames || quality.queueDroppedFrames || quality.invalidFrames || quality.discontinuities)
        alerts << QStringLiteral("存在缺口或无效帧");
    if (quality.unverifiedFrames) alerts << QStringLiteral("来源有效性未验证");
    if (quality.stoppedEarly) alerts << QStringLiteral("停止尾部未保证完整");
    if (quality.boundaryExcludedEvents) alerts << QStringLiteral("已排除边界窗口");
    if (quality.pendingWindowEvents) alerts << QStringLiteral("%1 个窗口待后续样本").arg(quality.pendingWindowEvents);
    m_qualitySummary->setText(QStringLiteral("ISI %1 · <2 ms %2 · %3")
        .arg(m_qc.intervalsMs.size()).arg(m_qc.shortIntervals)
        .arg(alerts.isEmpty() ? QStringLiteral("仅统计当前留存窗口") : alerts.join(QStringLiteral(" · "))));
    m_qualitySummary->setToolTip(m_quality->text());
    m_qualitySummary->setProperty("state", alerts.isEmpty() ? "info" : "warn");
    m_qualitySummary->style()->unpolish(m_qualitySummary);
    m_qualitySummary->style()->polish(m_qualitySummary);

}

void SpikeSortingWidget::updatePlots() { for (auto *plot : m_plots) plot->update(); }

void SpikeSortingWidget::selectRows(const QVector<int> &rows, bool append) {
    if (!append) m_selected.clear();
    for (int row : rows)
        if (row >= 0 && row < m_model.events().size()) m_selected.insert(m_model.events()[row].sequence);
    refreshSummary(); updatePlots();
}

void SpikeSortingWidget::assignSelected(int unit) {
    if (!m_store || m_selected.isEmpty()) return;
    const QVector<quint64> sequences(m_selected.cbegin(), m_selected.cend());
    const int applied = m_store->setCandidateUnit(m_snapshot.quality.epoch, m_lane, sequences, unit);
    refreshData();
    m_feedback->setText(QStringLiteral("已处理 %1 / %2 个所选事件：%3；跳过 %4 个已失效事件。标签仅随留存事件保存，请及时导出")
        .arg(applied).arg(sequences.size()).arg(unit < 0 ? QStringLiteral("移除标记") : QStringLiteral("标记候选 %1").arg(unit))
        .arg(sequences.size() - applied));
}

void SpikeSortingWidget::setWaveTheme(const QMap<QString, QString> &palette) {
    const auto pick = [&](const char *key, QColor &color) {
        const QColor candidate(palette.value(QLatin1String(key)));
        if (candidate.isValid()) color = candidate;
    };
    pick("appBg", m_windowBg); pick("plotBg", m_plotBg);
    pick("text", m_text); pick("title", m_secondary); pick("grid", m_grid);
    pick("axis", m_axis); pick("wave", m_wave); pick("border", m_border);
    pick("accent", m_accent); pick("warning", m_warning);
    update(); updatePlots();
}

void SpikeSortingWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    // QWidget's global QSS background is transparent. A real modeless window
    // needs its own opaque app surface, otherwise light-theme text lands on
    // an OS/default black backing instead of the application's theme.
    QPainter painter(this);
    painter.fillRect(rect(), m_windowBg);
}

void SpikeSortingWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); refreshData(); m_timer.start();
}
void SpikeSortingWidget::hideEvent(QHideEvent *event) {
    m_timer.stop(); QWidget::hideEvent(event);
}

} // namespace ccv2
