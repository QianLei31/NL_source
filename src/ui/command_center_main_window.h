#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QTimer>

#include <atomic>
#include <memory>

#include "config/config_manager.h"
#include "core/channel_address_state.h"
#include "core/network_state.h"
#include "core/threadsafe_queue.h"
#include "network/timestamp_checker.h"

class QThread;

namespace ccv2 {

class DummyStreamServer;
class SpiControlPanel;
class RealtimePlotPanel;
class AnalyzerPanel;
class ChannelMapPanel;
class SweepPlotPanel;
class SpikePanel;
class RecordingPanel;
class SessionToolbar;
class TopStatusBar;
class SettingsPanel;
class SystemStatusModel;
class ThemeManager;
class UpdateClient;
class SessionHub;
struct ThemePalette;

class CommandCenterMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit CommandCenterMainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void saveUiToConfig();
    void saveNetworkToConfig(const QString &host, int port, int dataPort);
    void saveLocalTestConfig(bool managedSource, int controlPort, int dataPort);
    bool startManagedDummy(int controlPort, int dataPort);
    void stopManagedDummy();
    void setEndpoint(const QString &host, int port, int dataPort, bool saveCfg, bool rememberManual);
    void onSettingsApplied(const QString &host, int port, int dataPort, const QString &theme);
    void applyPaletteToPages(const ThemePalette &palette);

    ConfigManager m_cfgMgr;
    NetworkState *m_networkState{nullptr};
    ChannelAddressState *m_channelState{nullptr};
    SessionHub *m_sessionHub{nullptr};
    TimestampMonitorWorker *m_tsMonitor{nullptr};
    std::shared_ptr<ThreadSafeQueue<QByteArray>> m_tsMonitorQueue;
    std::unique_ptr<std::atomic_bool> m_tsMonitorStop;
    QTimer *m_recTimer{nullptr};
    QElapsedTimer m_recElapsed;
    qint64 m_lastStorageCheckSec{-1};
    bool m_pendingRecord{false};
    bool m_triggeredRecordingActive{false};
    QString m_manualHost;
    int m_manualPort{10086};
    int m_manualDataPort{5001};
    bool m_localTestMode{false};
    bool m_managedLocalTest{false};
    bool m_managedDummySpike{false};

    // V6 shell
    TopStatusBar *m_statusBar{nullptr};
    SessionToolbar *m_sessionToolbar{nullptr};
    SettingsPanel *m_settingsPanel{nullptr};
    SystemStatusModel *m_statusModel{nullptr};
    ThemeManager *m_themeManager{nullptr};

    QStackedWidget *m_stack{nullptr};

    ChannelMapPanel *m_pageMap{nullptr};
    SpiControlPanel *m_pageSpi{nullptr};
    RecordingPanel *m_recordingPanel{nullptr};
    RealtimePlotPanel *m_pageRt{nullptr};
    AnalyzerPanel *m_pageAnalyzer{nullptr};
    SweepPlotPanel *m_pageSweep{nullptr};
    SpikePanel *m_pageSpike{nullptr};
    UpdateClient *m_updateClient{nullptr};
    DummyStreamServer *m_dummyServer{nullptr};
    QThread *m_dummyThread{nullptr};
    bool m_updateCheckedThisSession{false};
};

}  // namespace ccv2
