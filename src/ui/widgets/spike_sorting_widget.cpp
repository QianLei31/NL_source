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
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ccv2 {
namespace {
QString featureName(SpikeFeatureAxis axis) {
    switch (axis) {
    case SpikeFeatureAxis::PeakToPeak: return QStringLiteral("Peak-to-peak (µV)");
    case SpikeFeatureAxis::SignedPeak: return QStringLiteral("Signed peak (µV)");
    case SpikeFeatureAxis::Energy: return QStringLiteral("Energy (µV²·ms)");
    }
    return {};
}
QColor unitColor(int unit) {
    if (unit < 0) return QColor(143, 155, 173);
    return QColor::fromHsv((unit * 71 + 15) % 360, 175, 235);
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
        p.fillRect(rect(), QColor(20, 27, 37));
        p.setPen(QColor(72, 86, 104));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
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
            p.setPen(QPen(QColor(255, 222, 128), 1, Qt::DashLine));
            p.setBrush(QColor(255, 222, 128, 30));
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
    QPointF point(double x, double y) const {
        return {m_area.left() + (x - m_x0) / (m_x1 - m_x0) * m_area.width(),
                m_area.bottom() - (y - m_y0) / (m_y1 - m_y0) * m_area.height()};
    }
    void axes(QPainter &p, const QString &title, const QString &x, const QString &y, bool integerY = false) {
        p.setPen(QColor(220, 229, 240));
        p.drawText(QRectF(10, 5, width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, title);
        p.setPen(QColor(137, 153, 172));
        p.drawRect(m_area);
        QFont font = p.font(); font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1)); p.setFont(font);
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
        p.setPen(QColor(165, 176, 192)); p.drawText(m_area, Qt::AlignCenter | Qt::TextWordWrap, text);
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
        axes(p, QStringLiteral("Features • drag to select"), featureName(m_owner->xAxis()), featureName(m_owner->yAxis()));
        p.save(); p.setClipRect(m_area);
        for (int row : rows) {
            const auto &f = m_owner->m_model.features()[row];
            const double x = f.value(m_owner->xAxis()), y = f.value(m_owner->yAxis());
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            const auto &e = m_owner->m_model.events()[row];
            const QPointF pos = point(x, y); m_points.append({row, pos});
            const bool selected = m_owner->m_selected.contains(e.sequence);
            p.setPen(selected ? QPen(QColor(255, 239, 183), 1.5) : QPen(Qt::NoPen));
            p.setBrush(unitColor(e.unitId)); p.drawEllipse(pos, selected ? 4 : 2.7, selected ? 4 : 2.7);
        }
        p.restore();
        if (m_points.isEmpty()) empty(p, QStringLiteral("No finite features in this retained snapshot"));
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
        axes(p, QStringLiteral("Waveforms: ≤80 overlays; filter mean ±SD (n=%1)").arg(qc.templateEvents),
             QStringLiteral("Time from threshold crossing (ms)"), QStringLiteral("Input µV"));
        p.save(); p.setClipRect(m_area);
        p.setPen(QPen(QColor(100, 112, 127), 1, Qt::DashLine));
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
            band.closeSubpath(); p.fillPath(band, QColor(235, 241, 248, 35));
            p.setPen(QPen(QColor(247, 248, 250), 2)); p.drawPath(mean);
        }
        p.restore();
        if (qc.templateEvents == 0) empty(p, QStringLiteral("No compatible finite waveform template"));
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
        axes(p, QStringLiteral("Retained source-time raster • 0 = unassigned"),
             QStringLiteral("Source frame / source Fs (s)"), QStringLiteral("Candidate"), true);
        p.save(); p.setClipRect(m_area);
        for (int row : rows) {
            const auto &e = m_owner->m_model.events()[row];
            if (!e.hasSourceTime() || !std::isfinite(e.sourceSampleRate)) continue;
            const QPointF pos = point(e.sourceTimeSeconds(), std::max(0, e.unitId));
            p.setPen(QPen(unitColor(e.unitId), 1.5)); p.drawLine(pos + QPointF(0, -4), pos + QPointF(0, 4));
        }
        p.restore();
        if (timed == 0) empty(p, QStringLiteral("No source timestamps available"));
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
        axes(p, QStringLiteral("Retained ISI • 2 ms bins • ≥100 ms: %1").arg(overflow),
             QStringLiteral("Within-segment interval (ms)"), QStringLiteral("Count"), true);
        p.save(); p.setClipRect(m_area);
        for (int i = 0; i < bins; ++i) {
            QRectF bar(point(i * 2, counts[i]), point((i + 1) * 2, 0));
            p.fillRect(bar.normalized().adjusted(0.5, 0, -0.5, 0), i == 0 ? QColor(235, 159, 98) : QColor(91, 180, 201));
        }
        p.restore();
        if (qc.intervalsMs.isEmpty()) empty(p, QStringLiteral("Need ≥2 source-timed events in one continuity segment"));
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
    setWindowTitle(QStringLiteral("候选单元 / Manual candidate-unit workspace"));
    resize(1080, 820);
    auto *root = new QVBoxLayout(this);
    auto *notice = new QLabel(QStringLiteral(
        "Manual candidate labels only; no automatic biological-unit isolation. "
        "Work on the newest retained events of one lane. Labels expire when events are evicted or analysis resets; export before then."));
    notice->setWordWrap(true); root->addWidget(notice);
    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QStringLiteral("Lane (0-based)")));
    m_laneSpin = new QSpinBox; m_laneSpin->setObjectName(QStringLiteral("candidateLane"));
    m_laneSpin->setRange(0, std::max(0, store ? store->channels() - 1 : 0)); controls->addWidget(m_laneSpin);
    controls->addWidget(new QLabel(QStringLiteral("Show")));
    m_unitFilter = new QComboBox; m_unitFilter->setObjectName(QStringLiteral("candidateFilter"));
    m_unitFilter->addItem(QStringLiteral("All / pooled"), SpikeSortingModel::kAllUnits);
    m_unitFilter->addItem(QStringLiteral("Unassigned"), -1);
    for (int i = 1; i <= 32; ++i) m_unitFilter->addItem(QStringLiteral("Candidate %1").arg(i), i);
    controls->addWidget(m_unitFilter);
    m_freeze = new QCheckBox(QStringLiteral("Freeze snapshot"));
    m_freeze->setObjectName(QStringLiteral("candidateFreeze"));
    m_freeze->setToolTip(QStringLiteral("Freezes only this bounded view. Acquisition/detection continue; selected events may be evicted before labeling."));
    controls->addWidget(m_freeze);
    auto *refresh = new QPushButton(QStringLiteral("Refresh snapshot"));
    refresh->setObjectName(QStringLiteral("candidateRefresh")); controls->addWidget(refresh);
    controls->addStretch(); root->addLayout(controls);
    auto *featureControls = new QHBoxLayout;
    featureControls->addWidget(new QLabel(QStringLiteral("X"))); m_xAxis = new QComboBox;
    featureControls->addWidget(m_xAxis); featureControls->addWidget(new QLabel(QStringLiteral("Y"))); m_yAxis = new QComboBox;
    for (auto axis : {SpikeFeatureAxis::PeakToPeak, SpikeFeatureAxis::SignedPeak, SpikeFeatureAxis::Energy}) {
        m_xAxis->addItem(featureName(axis), int(axis)); m_yAxis->addItem(featureName(axis), int(axis));
    }
    m_yAxis->setCurrentIndex(1); featureControls->addWidget(m_yAxis);
    auto *selectAll = new QPushButton(QStringLiteral("Select shown")); selectAll->setObjectName(QStringLiteral("candidateSelectAll"));
    auto *clear = new QPushButton(QStringLiteral("Clear selection"));
    featureControls->addWidget(selectAll); featureControls->addWidget(clear); featureControls->addStretch();
    root->addLayout(featureControls);
    auto *grid = new QGridLayout;
    for (int i = 0; i < 4; ++i) {
        auto *plot = new SpikeSortingPlot(this, static_cast<SpikeSortingPlot::Kind>(i));
        m_plots.append(plot); grid->addWidget(plot, i / 2, i % 2);
    }
    root->addLayout(grid, 1);
    auto *labels = new QHBoxLayout;
    m_selection = new QLabel; labels->addWidget(m_selection); labels->addStretch();
    m_unitSpin = new QSpinBox; m_unitSpin->setObjectName(QStringLiteral("candidateUnit")); m_unitSpin->setRange(1, 32);
    m_unitSpin->setPrefix(QStringLiteral("Candidate ")); labels->addWidget(m_unitSpin);
    m_assign = new QPushButton(QStringLiteral("Label selected")); m_assign->setObjectName(QStringLiteral("candidateAssign")); labels->addWidget(m_assign);
    m_unassign = new QPushButton(QStringLiteral("Unassign selected")); m_unassign->setObjectName(QStringLiteral("candidateUnassign")); labels->addWidget(m_unassign);
    root->addLayout(labels);
    m_coverage = new QLabel; m_coverage->setWordWrap(true); m_coverage->setObjectName(QStringLiteral("candidateCoverage")); root->addWidget(m_coverage);
    m_quality = new QLabel; m_quality->setWordWrap(true); root->addWidget(m_quality);
    m_feedback = new QLabel(QStringLiteral("Drag in the feature plot to select; Shift adds. White waveform = mean ±1 SD of the shown filter. Retained-event JSON export on the Spike page includes candidate labels."));
    m_feedback->setWordWrap(true); m_feedback->setObjectName(QStringLiteral("candidateFeedback")); root->addWidget(m_feedback);
    auto *caution = new QLabel(QStringLiteral("QC uses retained events only. ISIs never bridge known continuity gaps. Detector refractory dead time, missed events and retention bias these metrics; a low short-ISI count does not establish unit isolation."));
    caution->setWordWrap(true); root->addWidget(caution);

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
            m_feedback->setText(QStringLiteral("Source epoch or lane changed; previous selection cleared."));
    }
    if (!m_model.setSnapshot(m_lane, snapshot.events, snapshot.waveforms, snapshot.snippetLength)) {
        m_selected.clear();
        m_feedback->setText(QStringLiteral("Snapshot unavailable or incompatible; refresh after analysis is configured."));
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
        m_feedback->setText(QStringLiteral("Source epoch changed: old selection discarded and snapshot refreshed."));
    }
}

void SpikeSortingWidget::refreshSummary() {
    m_qc = m_model.unitQc(unitFilter());
    m_selection->setText(QStringLiteral("%1 selected / %2 shown").arg(m_selected.size()).arg(m_qc.retainedEvents));
    m_assign->setEnabled(!m_selected.isEmpty()); m_unassign->setEnabled(!m_selected.isEmpty());
    const int retained = int(m_model.events().size());
    const qint64 omitted = std::max<qint64>(0, m_snapshot.totalDetected - retained);
    QString identity;
    if (retained) {
        const auto &e = m_model.events().first();
        identity = QStringLiteral(" • ADC %1 / electrode %2 / phase %3").arg(e.adcChannel).arg(e.electrode).arg(e.tdmPhase);
    }
    m_coverage->setText(QStringLiteral("Epoch %1%2 • retained %3 / %4 detected (capacity %5; %6 outside view) • processed lane samples %7 • noise RMS %8 µV")
        .arg(m_snapshot.quality.epoch).arg(identity).arg(retained).arg(m_snapshot.totalDetected)
        .arg(m_snapshot.capacity).arg(omitted).arg(m_snapshot.observedSamples).arg(number(m_snapshot.noiseRmsV * 1e6)));
    const auto &quality = m_snapshot.quality;
    const QString coverage = quality.incomplete()
        ? QStringLiteral("INCOMPLETE: gaps %1, missing frames %2, queue-dropped %3, invalid %4, unverified frames %5, pending windows %6, boundary-excluded events %7%8")
              .arg(quality.discontinuities).arg(quality.missingSourceFrames).arg(quality.queueDroppedFrames)
              .arg(quality.invalidFrames).arg(quality.unverifiedFrames).arg(quality.pendingWindowEvents)
              .arg(quality.boundaryExcludedEvents).arg(quality.stoppedEarly ? QStringLiteral(", stopped early") : QString())
        : QStringLiteral("No reported analysis gaps (retained window, not full-session events)");
    const QString timing = m_qc.firstSourceSeconds >= 0
        ? QStringLiteral("%1–%2 s").arg(number(m_qc.firstSourceSeconds), number(m_qc.lastSourceSeconds)) : QStringLiteral("n/a");
    m_quality->setText(QStringLiteral("%1\nShown-filter QC: source span %2 • ISIs %3 • <2 ms %4 • invalid waves %5 • unknown times %6 • incompatible templates %7")
        .arg(coverage, timing).arg(m_qc.intervalsMs.size()).arg(m_qc.shortIntervals)
        .arg(m_qc.invalidWaveforms).arg(m_qc.unknownTimes).arg(m_qc.incompatibleTemplates));
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
    m_feedback->setText(QStringLiteral("%1 of %2 selected events %3. %4 stale/evicted events skipped. Labels belong to retained events only; export before eviction/reset.")
        .arg(applied).arg(sequences.size()).arg(unit < 0 ? QStringLiteral("unassigned") : QStringLiteral("labeled candidate %1").arg(unit))
        .arg(sequences.size() - applied));
}

void SpikeSortingWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); refreshData(); m_timer.start();
}
void SpikeSortingWidget::hideEvent(QHideEvent *event) {
    m_timer.stop(); QWidget::hideEvent(event);
}

} // namespace ccv2
