#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <memory>

#include "config/config_manager.h"
#include "core/channel_address_state.h"
#include "core/network_state.h"
#include "core/threadsafe_queue.h"
#include "network/data_sorter.h"

namespace ccv2 {

class ActivityMapView;
class StackWaveformView;
class WaveformWidget;
class SessionHub;

class RealtimePlotPanel : public QWidget {
    Q_OBJECT

public:
    explicit RealtimePlotPanel(ConfigManager *cfgMgr,
                               NetworkState *networkState,
                               ChannelAddressState *channelState,
                               QWidget *parent = nullptr);
    void shutdown();
    void setWaveTheme(const QMap<QString, QString> &palette);
    void setSessionHub(SessionHub *hub);
    void onActivated();
    void onDeactivated();

private slots:
    void startStream();
    void stopStream();
    void pauseStream();
    void resumeStream();
    void refresh();
    void applyChannels();
    void applyViewLength();
    void onNetworkChanged(const QString &host, int port);
    void onGetChannelAddress();

private:
    QVector<int> parseChannels() const;
    void buildUi(const QVector<int> &defaultChannels);
    bool applyChannelsInternal();
    bool applyViewLengthInternal(bool showMessage);
    int refreshIntervalMs() const;
    void updateTimeHint();
    bool stackMode() const;
    int displaySignalMode() const;
    int stackYScaleMode() const;
    void updateStackYScale();
    QVector<int> effectiveChannelsForMode(const QVector<int> &channels) const;
    void updateOriginPlots(const QVector<int> &channels,
                           const QVector<QVector<double>> &samples,
                           const QVector<qint64> &firstSampleIndices,
                           bool tdmMode,
                           bool evenSampleIsFirstLocalEle);
    void updateStackPlots(const QVector<int> &channels,
                          const QVector<QVector<double>> &samples,
                          const QVector<qint64> &firstSampleIndices,
                          bool tdmMode,
                          bool evenSampleIsFirstLocalEle);
    void updateActivityMap(const QVector<int> &selectedChannels,
                           const QVector<RealtimeChannelMetric> &metrics,
                           const QVector<RealtimeChannelMetric> &tdmMetrics,
                           bool tdmMode,
                           bool evenSampleIsFirstLocalEle,
                           quint64 metricsEpoch);
    void selectHeatmapWindow(int centerChannel);
    void saveRealtimeConfig() const;
    // Debounced entry point for edit handlers: every control change used to
    // rewrite the whole INI synchronously on the GUI thread.
    void scheduleSaveRealtimeConfig();
    bool stopThreads();
    void resetTimelineState(quint64 epoch, qint64 targetFrame);
    // Mirror the global TdmContext into this page's controls and displays.
    void applyTdmState(bool enabled, bool evenFirst);

    QString m_defaultHost;
    ConfigManager *m_cfgMgr;
    double m_samplingRate{20000.0};
    int m_samplesPerView{1200};
    QString m_defaultCommand{QStringLiteral("ctre")};
    QString m_defaultChannelsText;
    int m_defaultRefreshHz{10};
    int m_defaultViewMode{1};
    int m_defaultSignalMode{0};
    int m_defaultRefMode{0};
    int m_defaultStackYScaleMode{0};
    int m_defaultHeatmapMetric{0};
    NetworkState *m_networkState;
    ChannelAddressState *m_channelState;
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_rawQueue;
    std::shared_ptr<RealtimeStreamState> m_streamState;

    std::unique_ptr<std::atomic_bool> m_stopFlag;
    QPointer<SessionHub> m_hub;  // hub dies before the page tree on shutdown
    QThread *m_receiver{nullptr};  // unused in V6 (kept for stop machinery)
    DataSorter *m_sorter{nullptr};

    bool m_paused{false};
    bool m_streaming{false};
    bool m_firstRefreshPending{false};
    qint64 m_rxBytes{0};
    qint64 m_parsedFrames{0};
    qint64 m_droppedFrames{0};
    int m_leftoverBytes{0};
    int m_lastBufferSamples{0};

    QLabel *m_netLabel{nullptr};
    QLineEdit *m_rtCmd{nullptr};
    QLineEdit *m_rtChEdit{nullptr};
    QComboBox *m_viewModeCombo{nullptr};
    QComboBox *m_signalModeCombo{nullptr};
    QComboBox *m_refModeCombo{nullptr};
    QComboBox *m_stackYScaleCombo{nullptr};
    QComboBox *m_heatmapModeCombo{nullptr};
    QCheckBox *m_tdmCheckbox{nullptr};
    QComboBox *m_tdmPhaseCombo{nullptr};
    QSpinBox *m_rtPointsSpin{nullptr};
    QSpinBox *m_refreshHzSpin{nullptr};
    QLabel *m_rtTimeHint{nullptr};
    QLabel *m_rtStatus{nullptr};
    QPushButton *m_btnGetAddrRt{nullptr};
    QPushButton *m_btnApplyAxis{nullptr};

    QPushButton *m_btnApply{nullptr};

    QVector<WaveformWidget *> m_waveWidgets;
    QStackedWidget *m_plotModeStack{nullptr};
    StackWaveformView *m_stackLeftView{nullptr};
    StackWaveformView *m_stackRightView{nullptr};
    ActivityMapView *m_activityMap{nullptr};
    QLabel *m_activitySummary{nullptr};
    quint64 m_lastMetricsEpoch{0};
    QTimer m_refreshTimer;
    QTimer m_saveDebounceTimer;
};

}  // namespace ccv2
