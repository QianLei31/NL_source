#pragma once

#include "io/spike_event_archive.h"
#include "signal/spike_rule_classifier.h"
#include <QColor>
#include <QMap>
#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace ccv2 {
class SpikeRulePlot;

// Edits definitions and previews captured waveforms only. The processing service
// assigns actual immutable revisions/boundaries; this view never classifies the
// incoming stream, changes detector settings, or starts acquisition.
class SpikeRuleEditor : public QWidget {
    Q_OBJECT
public:
    explicit SpikeRuleEditor(QWidget *parent = nullptr);
    void setLane(int lane);
    int lane() const { return m_lane; }
    void setCapturedContext(const SpikeRuleContext &context,
                            const QVector<SpikeArchiveRecord> &waveforms,
                            const QString &status = {});
    void setAppliedRules(SpikeRuleSet::Snapshot rules, const QString &status = {});
    void setRequestedRules(SpikeRuleSet::Snapshot rules);
    void setServiceStatus(const QString &status);
    void setWaveTheme(const QMap<QString, QString> &palette);
    QVector<SpikeElectrodeRules> definitions() const { return m_definitions; }
    bool loadRules(const QString &path);
    bool saveRules(const QString &path);
    bool addBox(int unitId, const SpikeWaveformBox &box);
    void requestApply();

signals:
    void captureRequested(int lane);
    void applyRulesRequested(const QVector<ccv2::SpikeElectrodeRules> &definitions);

protected:
    void paintEvent(QPaintEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    friend class SpikeRulePlot;
    int electrodeIndex() const;
    void refresh();
    void removeSelectedBox();
    void clearElectrode();
    void bindCapturedContext();
    void markEdited();
    int m_lane = 0;
    bool m_haveContext = false;
    bool m_dirty = false;
    SpikeRuleContext m_context;
    QVector<SpikeArchiveRecord> m_waveforms;
    QVector<SpikeElectrodeRules> m_definitions;
    SpikeRuleSet::Snapshot m_applied;
    SpikeRuleSet::Snapshot m_requested;
    QSpinBox *m_laneSpin = nullptr;
    QSpinBox *m_unit = nullptr;
    QDoubleSpinBox *m_timeFrom = nullptr;
    QDoubleSpinBox *m_timeTo = nullptr;
    QDoubleSpinBox *m_voltageFrom = nullptr;
    QDoubleSpinBox *m_voltageTo = nullptr;
    QListWidget *m_boxes = nullptr;
    QLabel *m_contextLabel = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_revision = nullptr;
    QLabel *m_feedback = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_clear = nullptr;
    QPushButton *m_bind = nullptr;
    SpikeRulePlot *m_plot = nullptr;
    QColor m_windowBg{"#15171A"}, m_plotBg{"#131518"}, m_border{"#31363D"};
    QColor m_text{"#E8EAED"}, m_secondary{"#A8B0BA"}, m_axis{"#8A929C"};
    QColor m_wave{"#4FB6C4"}, m_accent{"#4D8FE8"}, m_grid{"#262B31"};
};
} // namespace ccv2
