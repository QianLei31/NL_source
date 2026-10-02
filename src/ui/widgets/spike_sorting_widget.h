#pragma once

#include <QSet>
#include <QTimer>
#include <QWidget>

#include "signal/spike_snippet_store.h"
#include "signal/spike_sorting_model.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QShowEvent;
class QHideEvent;

namespace ccv2 {

class SpikeSortingPlot;

// Reusable modeless manual candidate-unit workspace. It reads the existing
// detector's bounded store and adds no acquisition or signal-processing worker.
class SpikeSortingWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpikeSortingWidget(SpikeSnippetStore *store, QWidget *parent = nullptr);
    void setLane(int lane);
    int lane() const { return m_lane; }
    void refreshData();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    friend class SpikeSortingPlot;
    void refreshIfLive();
    void refreshSummary();
    void updatePlots();
    void assignSelected(int unit);
    void selectRows(const QVector<int> &rows, bool append);
    int unitFilter() const;
    SpikeFeatureAxis xAxis() const;
    SpikeFeatureAxis yAxis() const;

    SpikeSnippetStore *m_store;
    SpikeSortingModel m_model;
    SpikeUnitQc m_qc;
    SpikeLaneSnapshot m_snapshot;
    QSet<quint64> m_selected;
    int m_lane = 0;
    QTimer m_timer;
    QSpinBox *m_laneSpin = nullptr;
    QSpinBox *m_unitSpin = nullptr;
    QComboBox *m_unitFilter = nullptr;
    QComboBox *m_xAxis = nullptr;
    QComboBox *m_yAxis = nullptr;
    QCheckBox *m_freeze = nullptr;
    QLabel *m_coverage = nullptr;
    QLabel *m_quality = nullptr;
    QLabel *m_selection = nullptr;
    QLabel *m_feedback = nullptr;
    QPushButton *m_assign = nullptr;
    QPushButton *m_unassign = nullptr;
    QVector<SpikeSortingPlot *> m_plots;
};

} // namespace ccv2
