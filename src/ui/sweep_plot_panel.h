#pragma once

#include <QPointer>
#include <QWidget>
#include <QTimer>
#include <QMap>
#include <QVector>
#include <QString>

#include <atomic>
#include <memory>

#include "core/threadsafe_queue.h"
#include "network/data_sorter.h"
#include "signal/spike_filter.h"

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QByteArray;

namespace ccv2 {

class ConfigManager;
class NetworkState;
class SocketReceiver2;
class SessionHub;
class SweepWaveformView;
class ActivityMapView;

// Page 4: continuous multi-channel sweep oscilloscope (Intan-style refresh).
// Self-contained data path (own receiver + sorter + stream state) so it does
// not touch the realtime/analyzer pipelines.
class SweepPlotPanel : public QWidget {
    Q_OBJECT
public:
    explicit SweepPlotPanel(ConfigManager *cfgMgr, NetworkState *networkState, QWidget *parent = nullptr);
    ~SweepPlotPanel() override;

    void setWaveTheme(const QMap<QString, QString> &palette);
    void setSessionHub(SessionHub *hub);
    void onActivated();    // page shown + session running -> subscribe & display
    void onDeactivated();  // page hidden -> unsubscribe
    void stopAcquisition();
    void shutdown();

private slots:
    void startStream();
    void stopStream();
    void togglePause();
    void refresh();

private:
    void buildUi();
    bool stopThreads();
    QVector<int> parseChannels(const QString &text) const;
    void setDisplayChannels(const QVector<int> &chs);  // live channel switch
    void applyViewParams();
    void applyProcConfig();
    void updateFrequencyLimits();
    void syncSourceSampleRate();
    double sourceSampleRate() const;
    void loadConfig();
    void scheduleSaveConfig();
    void saveConfig() const;
    void completeStopWhenReady(const QString &finalText,
                               const QString &state,
                               quint64 lifecycleGeneration);
    void setStatus(const QString &text, const char *state);
    void resetTimelineState(quint64 epoch, qint64 targetFrame);
    // Mirror the global TdmContext into this page and reconfigure the display
    // pipeline (lane count, decimated rate) when the mode/phase changes.
    void applyTdmState(bool enabled, bool evenFirst);
    // Per-lane view identity: raw ADC channels normally, or 2x ELE numbers in
    // TDM mode (lane 2i / 2i+1 = channel i's first / second electrode).
    QVector<int> displayLanes() const;
    int laneCount() const;             // channels * (tdm ? 2 : 1)
    double procSampleRate() const;     // fs, divided by four in TDM mode

    ConfigManager *m_cfgMgr;
    NetworkState *m_networkState;

    SweepWaveformView *m_view = nullptr;
    ActivityMapView *m_activityMap = nullptr;
    QComboBox *m_metricCombo = nullptr;
    QComboBox *m_clickModeCombo = nullptr;
    QCheckBox *m_tdmCheck = nullptr;
    QComboBox *m_tdmPhaseCombo = nullptr;
    QLineEdit *m_chEdit = nullptr;
    QLineEdit *m_cmdEdit = nullptr;
    QDoubleSpinBox *m_spanSpin = nullptr;
    QDoubleSpinBox *m_fsSpin = nullptr;
    double m_standaloneSampleRate = 20000.0;
    QDoubleSpinBox *m_yfsSpin = nullptr;
    QSpinBox *m_refreshSpin = nullptr;
    QComboBox *m_bandCombo = nullptr;
    QComboBox *m_notchCombo = nullptr;
    QSpinBox *m_hpSpin = nullptr;
    QSpinBox *m_spikeLpSpin = nullptr;
    QComboBox *m_orderCombo = nullptr;
    QComboBox *m_threshModeCombo = nullptr;
    QDoubleSpinBox *m_threshSpin = nullptr;
    QComboBox *m_polarityCombo = nullptr;
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnStop = nullptr;
    QPushButton *m_btnPause = nullptr;
    QLabel *m_status = nullptr;

    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    std::shared_ptr<RealtimeStreamState> m_streamState;
    std::unique_ptr<std::atomic_bool> m_stopFlag;
    // QPointer: SessionHub is created before (and thus destroyed before) the
    // central-widget page tree; ~SweepPlotPanel runs shutdown() and must see
    // null here instead of a dangling pointer.
    QPointer<SessionHub> m_hub;
    SocketReceiver2 *m_receiver = nullptr;  // unused in V6 (kept for stop machinery)
    DataSorter *m_sorter = nullptr;

    QTimer m_refreshTimer;
    QTimer m_saveTimer;
    SpikeProcessor m_proc;
    bool m_loadingConfig = false;
    bool m_streaming = false;
    bool m_paused = false;
    bool m_firstRefreshPending = false;
    bool m_tdmEnabled = false;
    bool m_tdmEvenFirst = true;
    quint64 m_lifecycleGeneration = 0;
    QVector<int> m_channels;
    QMap<int, qint64> m_lastTotal;

    qint64 m_rxBytes = 0;
    qint64 m_parsedFrames = 0;
    qint64 m_droppedFrames = 0;
};

}  // namespace ccv2
