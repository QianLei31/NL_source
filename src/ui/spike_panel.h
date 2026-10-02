#pragma once

#include <QMap>
#include <QJsonObject>
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
class SpikeSortingWidget;

// Page 5: array-wide spike panel (Blackrock Central style). Every electrode
// gets a cell that overlays the last N threshold-aligned waveforms, so unit
// activity across all 256/512 sites is visible at a glance; clicking a cell
// opens a non-modal single-channel inspector window.
//
// Detection follows the session lifetime, including while this page is hidden
// or its display is paused. Only rendering follows tab visibility. Detection
// runs on a dedicated thread; the GUI reads bounded snapshots from its store.
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

    // Read-only access for same-session analysis consumers and diagnostics.
    // The store's snapshot methods are thread-safe; its lifetime is this panel.
    const SpikeSnippetStore *analysisStore() const { return &m_store; }
    bool analysisRunning() const { return m_running; }
    QVector<double> analysisRatesHz() const { return m_rates; }

private slots:
    void refresh();
    void restartDetection();   // config change -> rebuild the worker
    void clearAll();
    void exportSelectedLane();

private:
    void buildUi();
    bool stopWorker();
    bool configureAnalysisStore(const SpikeDetectConfig &cfg);
    void startDetection();
    void syncSessionState();
    void resetViewState();
    void publishAnalysisMetadata(bool newInterval = false);
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
    double procSampleRate() const;   // one lane sample per four source frames in TDM
    QMap<int, double> &activeThresholdOverrides();
    const QMap<int, double> &activeThresholdOverrides() const;
    int thresholdKeyForLane(int lane) const;

    ConfigManager *m_cfgMgr;
    QPointer<SessionHub> m_hub;

    SpikeGridView *m_grid = nullptr;
    SpikeDetailWindow *m_detailWindow = nullptr;
    SpikeSortingWidget *m_sortingWindow = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_qualityStatus = nullptr;
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
    QPushButton *m_btnExport = nullptr;

    SpikeSnippetStore m_store;
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    std::unique_ptr<std::atomic_bool> m_stopFlag;
    SpikeDetectWorker *m_worker = nullptr;

    QTimer m_refreshTimer;
    QTimer m_saveTimer;
    QVector<double> m_rates;
    double m_analysisSampleRate = 20000.0;
    bool m_running = false;
    bool m_viewActive = false;
    bool m_shutdown = false;
    bool m_paused = false;
    bool m_loadingConfig = false;
    QString m_analysisError;
    QString m_replayProvenanceNote;
    QJsonObject m_replayAnalysisMetadata;
    QJsonObject m_analysisProvenance;
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
