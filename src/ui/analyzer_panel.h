#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFuture>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <memory>

#include "config/config_manager.h"
#include "core/channel_address_state.h"
#include "core/network_state.h"
#include "core/threadsafe_queue.h"
#include "data/data_store.h"
#include "network/sorter_worker.h"

namespace ccv2 {

class WaveformWidget;
class FftStatsTable;
class MultiChannelStatsTable;
class SessionHub;

struct RuntimeConfig {
    QString host{QStringLiteral("localhost")};
    int port{10086};
    double samplingRate{20000.0};
    double refreshHz{10.0};
    int wavePoints{8000};
    bool fftEnabled{true};
    int fftPoints{2048};
    QString fftWindow{QStringLiteral("hann")};
    int fftDcBins{5};
    int fftSigBins{5};
    double fftBandwidthHz{0.0};
    double irnGain{60.0};
    QString fftYMode{QStringLiteral("psd_db")};  // psd_db | density (V/√Hz)
    QString channels{QStringLiteral("239,240")};
    QString statsSource;
    bool pauseKeepCapture{true};
    QString saveDir{QStringLiteral("d:/ADC_data")};
};

class AnalyzerPanel : public QWidget {
    Q_OBJECT

public:
    explicit AnalyzerPanel(ConfigManager *cfgMgr,
                           NetworkState *networkState,
                           ChannelAddressState *channelState,
                           QWidget *parent = nullptr);
    void stopAcquisition();
    void shutdown();
    void setPlotTheme(const QMap<QString, QString> &palette);
    void setSessionHub(SessionHub *hub);
    void onActivated();
    void onDeactivated();
    double configuredLiveSampleRate() const { return m_cfg.samplingRate; }

private slots:
    void startLive();
    void stopAll();
    void updatePlots();
    void onNetworkChanged(const QString &host, int port);
    void onGetChannelAddress();
    void checkNetworkConnectivity();

private:
    QPair<QVector<QVector<int>>, QVector<int>> parseChannelGroups(const QString &text) const;
    QPair<QVector<QVector<int>>, QVector<int>> getChannels() const;
    QPair<QVector<QVector<int>>, QVector<int>> initBuffersAndCurves();
    void setupThreads(const QVector<int> &channels);
    void startTimer();
    void resetFftStats();
    void resetTimelineState(quint64 epoch, qint64 targetFrame);
    void updateStatsSourceChoices(const QStringList &labels);
    QString selectedStatsSourceLabel(const QStringList &labels) const;
    void setAllChannelsOverviewMode(bool enabled);
    void restartPipelineIfRunning();
    int fftPointCount() const;
    void syncFftUiState();
    void applyFftYAxisMode();
    bool fftDensityMode() const;
    void updateModeStatus();
    int maxSelectableChannels() const;
    void scheduleSaveAnalyzerConfig();
    void saveAnalyzerConfigNow() const;
    // Mirror the global TdmContext into this page's controls (no-op while the
    // 256-channel overview forces TDM off locally).
    void applyGlobalTdmState(bool enabled, bool evenFirst);

    RuntimeConfig m_cfg;
    ConfigManager *m_cfgMgr;
    NetworkState *m_networkState;
    ChannelAddressState *m_channelState;

    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    std::unique_ptr<std::atomic_bool> m_stopFlag;
    DataStore m_dataStore;

    QPointer<SessionHub> m_hub;  // hub dies before the page tree on shutdown
    SorterWorker *m_sorter{nullptr};

    bool m_isRunning{false};
    bool m_isPaused{false};
    bool m_firstRefreshPending{false};
    bool m_allChannelsOverviewMode{false};
    bool m_overviewSavedFftEnabled{true};
    double m_overviewSavedRefreshHz{10.0};
    int m_overviewSavedWavePoints{8000};
    QString m_overviewSavedChannels{QStringLiteral("239,240")};
    qint64 m_rxBytes{0};
    qint64 m_parsedFrames{0};
    qint64 m_droppedFrames{0};
    int m_leftoverBytes{0};
    int m_lastBufferSamples{0};
    std::atomic<quint64> m_timelineEpoch{0};

    QVector<QVector<int>> m_groups;
    QVector<int> m_channels;

    QLabel *m_netLabel{nullptr};
    QLabel *m_netStateLabel{nullptr};
    QLabel *m_modeStatusLabel{nullptr};
    QLineEdit *m_channelsEdit{nullptr};
    QPushButton *m_btnAllChannelsOverview{nullptr};
    QCheckBox *m_tdmCheckbox{nullptr};
    QComboBox *m_tdmPhaseCombo{nullptr};
    QCheckBox *m_pauseKeepCaptureCheckbox{nullptr};
    QComboBox *m_statsSourceCombo{nullptr};
    QDoubleSpinBox *m_refreshSpin{nullptr};
    QDoubleSpinBox *m_fsSpin{nullptr};
    QSpinBox *m_wavePointsSpin{nullptr};
    QCheckBox *m_fftEnabledCheckbox{nullptr};
    QComboBox *m_fftPointsCombo{nullptr};
    QComboBox *m_fftWindowCombo{nullptr};
    QSpinBox *m_fftDcBinsSpin{nullptr};
    QSpinBox *m_fftSigBinsSpin{nullptr};
    QDoubleSpinBox *m_fftBandwidthSpin{nullptr};
    QDoubleSpinBox *m_irnGainSpin{nullptr};
    QComboBox *m_fftYModeCombo{nullptr};
    QPushButton *m_btnGetAddrAnalyzer{nullptr};
    QPushButton *m_btnProbeNetwork{nullptr};

    QLabel *m_statusLabel{nullptr};
    WaveformWidget *m_wavePlot{nullptr};
    WaveformWidget *m_fftPlot{nullptr};
    QWidget *m_timeStatsPanel{nullptr};
    MultiChannelStatsTable *m_timeStats{nullptr};
    QWidget *m_fftStatsPanel{nullptr};
    FftStatsTable *m_fftStats{nullptr};

    QTimer m_timer;
    QTimer m_saveDebounceTimer;
    std::atomic_bool m_shuttingDown{false};
    std::atomic_bool m_processEnabledAtomic{true};
    std::atomic_bool m_fftBusy{false};  // prevent overlapping FFT jobs
    // Bumped on every semantic analysis change; in-flight FFT jobs carry the
    // value they started with and are discarded on mismatch.
    std::atomic<quint64> m_analysisGeneration{0};
    QFuture<void> m_fftFuture;
};

}  // namespace ccv2
