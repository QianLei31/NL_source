#pragma once

#include <QElapsedTimer>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>

#include "core/threadsafe_queue.h"
#include "service/spike_detect_worker.h"
#include "signal/spike_snippet_store.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace ccv2 {

class ConfigManager;
class SessionHub;
class SpikeDetailWindow;
class SpikeGridView;

// Page 5: array-wide spike panel (Blackrock Central style). Every electrode
// gets a cell that overlays the last N threshold-aligned waveforms, so unit
// activity across all 256/512 sites is visible at a glance; clicking a cell
// opens a non-modal single-channel inspector window.
//
// Detection runs on a dedicated worker thread (filtering the full array at
// 20 kHz is far too heavy for the GUI thread); this page only pulls finished
// snippets out of the shared store on its refresh timer.
class SpikePanel : public QWidget {
    Q_OBJECT
public:
    explicit SpikePanel(ConfigManager *cfgMgr, QWidget *parent = nullptr);
    ~SpikePanel() override;

    void setSessionHub(SessionHub *hub);
    void setWaveTheme(const QMap<QString, QString> &palette);
    void onActivated();
    void onDeactivated();
    void shutdown();

private slots:
    void refresh();
    void restartDetection();   // config change -> rebuild the worker
    void clearAll();

private:
    void buildUi();
    bool stopWorker();
    void startDetection();
    void applyTdmState(bool enabled, bool pair02);
    void updateLaneLabels();
    void setStatus(const QString &text, const char *state);
    void showLaneDetail(int lane);
    void setLaneThresholdOverride(int lane, bool enabled, double magnitudeUv);
    void loadConfig();
    void scheduleSaveConfig();
    void saveConfig() const;
    SpikeDetectConfig currentConfig() const;
    int laneCount() const;
    double procSampleRate() const;   // halved in TDM mode
    QMap<int, double> &activeThresholdOverrides();
    const QMap<int, double> &activeThresholdOverrides() const;
    int thresholdKeyForLane(int lane) const;

    ConfigManager *m_cfgMgr;
    QPointer<SessionHub> m_hub;

    SpikeGridView *m_grid = nullptr;
    SpikeDetailWindow *m_detailWindow = nullptr;
    QLabel *m_status = nullptr;
    QComboBox *m_gainCombo = nullptr;
    QComboBox *m_threshModeCombo = nullptr;
    QDoubleSpinBox *m_threshSpin = nullptr;
    QComboBox *m_polarityCombo = nullptr;
    QSpinBox *m_hpSpin = nullptr;
    QComboBox *m_notchCombo = nullptr;
    QDoubleSpinBox *m_preSpin = nullptr;
    QDoubleSpinBox *m_postSpin = nullptr;
    QSpinBox *m_retainSpin = nullptr;
    QDoubleSpinBox *m_yScaleSpin = nullptr;
    QCheckBox *m_autoScaleCheck = nullptr;
    QCheckBox *m_fadeCheck = nullptr;
    QSpinBox *m_columnsSpin = nullptr;
    QComboBox *m_sortCombo = nullptr;
    QPushButton *m_btnClear = nullptr;
    QPushButton *m_btnPause = nullptr;

    SpikeSnippetStore m_store;
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    std::unique_ptr<std::atomic_bool> m_stopFlag;
    SpikeDetectWorker *m_worker = nullptr;

    QTimer m_refreshTimer;
    QTimer m_saveTimer;
    QElapsedTimer m_rateTimer;
    QVector<qint64> m_lastCounts;
    QVector<double> m_rates;
    qint64 m_framesProcessed = 0;
    bool m_running = false;
    bool m_paused = false;
    bool m_loadingConfig = false;
    bool m_tdmEnabled = false;
    bool m_tdmPair02 = true;
    int m_selectedLane = -1;
    double m_detailYScaleUv = 200.0;
    bool m_detailAutoScale = false;
    bool m_detailFade = true;
    int m_detailOverlayCount = 100;
    QMap<int, double> m_adcThresholdOverridesUv;
    QMap<int, double> m_tdmThresholdOverridesUv;
};

}  // namespace ccv2
