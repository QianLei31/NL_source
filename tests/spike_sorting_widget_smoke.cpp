#include "signal/spike_snippet_store.h"
#include "ui/widgets/spike_sorting_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char *what) { if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; } }
void add(ccv2::SpikeSnippetStore &store, quint64 epoch, int count) {
    for (int i = 0; i < count; ++i) {
        ccv2::SpikeEvent e; e.epoch = epoch; e.sourceFrame = 1000 + 100 * i;
        e.sourceSampleRate = 20000; e.adcChannel = 0; e.electrode = 0; e.preSamples = 1;
        const float wave[] = {0, -100e-6f, 30e-6f, 0};
        check(store.addEventIfEpoch(0, wave, e), "UI fixture event accepted");
    }
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    ccv2::SpikeSnippetStore store; store.configure(2, 4, 10); store.resetTimeline(5); add(store, 5, 4);
    ccv2::SpikeSortingWidget view(&store);
    view.resize(1080, 820); view.show(); app.processEvents();
    auto *all = view.findChild<QPushButton *>(QStringLiteral("candidateSelectAll"));
    auto *assign = view.findChild<QPushButton *>(QStringLiteral("candidateAssign"));
    auto *unassign = view.findChild<QPushButton *>(QStringLiteral("candidateUnassign"));
    auto *unit = view.findChild<QSpinBox *>(QStringLiteral("candidateUnit"));
    auto *freeze = view.findChild<QCheckBox *>(QStringLiteral("candidateFreeze"));
    auto *filter = view.findChild<QComboBox *>(QStringLiteral("candidateFilter"));
    check(all && assign && unassign && unit && freeze && filter, "workspace controls available");
    if (!all || !assign || !unassign || !unit || !freeze || !filter) return 1;
    check(!assign->isEnabled(), "no implicit selection on open");
    auto *plot = view.findChild<QWidget *>(QStringLiteral("candidateFeaturePlot"));
    check(plot != nullptr, "feature scatter plot available");
    if (plot) {
        const QPointF begin(65, 32), end(plot->width() - 21, plot->height() - 48);
        QMouseEvent press(QEvent::MouseButtonPress, begin, begin, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(plot, &press); QApplication::sendEvent(plot, &release);
        check(assign->isEnabled() && freeze->isChecked(), "rectangle selection freezes and selects plotted events");
    }
    all->click(); check(freeze->isChecked() && assign->isEnabled(), "selection freezes view only");
    unit->setValue(2); assign->click();
    for (const auto &e : store.snapshotLane(0).events) check(e.unitId == 2, "UI manual labels persisted");
    view.hide(); view.show(); app.processEvents();
    for (const auto &e : store.snapshotLane(0).events) check(e.unitId == 2, "modeless reopen preserves labels");
    filter->setCurrentIndex(1); all->click(); check(!assign->isEnabled(), "unassigned filter excludes labeled candidates");
    filter->setCurrentIndex(3); all->click(); unassign->click();
    for (const auto &e : store.snapshotLane(0).events) check(e.unitId == -1, "UI unassign persisted");
    check(!assign->isEnabled(), "events removed from visible filter do not remain secretly selected");
    filter->setCurrentIndex(0); all->click();
    store.resetTimeline(6); add(store, 6, 3);
    // No refresh before click: the store still rejects an old frozen selection.
    assign->click();
    for (const auto &e : store.snapshotLane(0).events) check(e.unitId == -1, "stale frozen epoch cannot label new events");
    check(!assign->isEnabled(), "new epoch clears selection");
    view.setLane(1); all->click(); check(!assign->isEnabled(), "empty lane is safe");
    view.setLane(0); all->click();
    add(store, 6, 12); assign->click();
    for (const auto &e : store.snapshotLane(0).events) check(e.unitId == -1, "evicted frozen selection cannot label replacement events");
    if (qEnvironmentVariableIsSet("CCV2_SORTING_SCREENSHOT")) {
        // Deterministic three-morphology, manually labeled visual QA fixture.
        store.configure(2, 33, 120); store.resetTimeline(7);
        QVector<quint64> labels[3];
        for (int i = 0; i < 96; ++i) {
            ccv2::SpikeEvent e; e.epoch = 7; e.sourceFrame = 100000 + i * 178 + (i % 3) * 20;
            e.sourceSampleRate = 20000; e.adcChannel = 0; e.electrode = 0; e.preSamples = 8;
            const int group = i % 3;
            float wave[33];
            for (int j = 0; j < 33; ++j) {
                const double trough = std::exp(-std::pow((j - 10) / 2.8, 2));
                const double rebound = std::exp(-std::pow((j - 17) / 4.2, 2));
                const double scale = 1 + 0.035 * std::sin(i * 3.1);
                const double uv = group == 0 ? -100 * trough + 25 * rebound :
                                  group == 1 ? 80 * trough - 30 * rebound : -50 * trough + 48 * rebound;
                wave[j] = float((scale * uv + 0.8 * std::sin(i + j * 2.2)) * 1e-6);
            }
            store.addEventIfEpoch(0, wave, e);
            labels[group].append(store.snapshotLane(0).events.last().sequence);
        }
        for (int group = 0; group < 3; ++group) store.setCandidateUnit(7, 0, labels[group], group + 1);
        store.addCoverageIfEpoch(0, 30000, 100000, 129999, 7);
        store.setStats({-50e-6, -50e-6}, {5e-6, 5e-6});
        view.refreshData(); app.processEvents();
    }
    const auto image = view.grab(); check(!image.isNull(), "four plots render offscreen");
    if (qEnvironmentVariableIsSet("CCV2_SORTING_SCREENSHOT")) image.save(qEnvironmentVariable("CCV2_SORTING_SCREENSHOT"));
    view.close(); view.show(); app.processEvents(); view.close();
    if (failures) return 1;
    std::cout << "spike_sorting_widget_smoke OK\n";
    return 0;
}
