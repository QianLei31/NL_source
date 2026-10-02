#include "ui/command_center_main_window.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QDateTime>
#include <QScreen>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>

#include "theme/theme_manager.h"
#include "core/app_version.h"
#include "core/tdm_context.h"
#include "service/dummy_stream_server.h"
#include "service/session_hub.h"
#include "service/update_client.h"
#include "io/session_recorder.h"
#include "ui/channel_map_panel.h"
#include "ui/realtime_plot_panel.h"
#include "ui/spi_control_panel.h"
#include "ui/analyzer_panel.h"
#include "ui/sweep_plot_panel.h"
#include "ui/spike_panel.h"
#include "ui/widgets/top_status_bar.h"
#include "ui/widgets/session_toolbar.h"
#include "ui/widgets/recording_panel.h"
#include "ui/widgets/settings_panel.h"
#include "model/system_status_model.h"

namespace ccv2 {

namespace {

QString cssColor(const QColor &color) {
    return color.isValid() ? color.name(QColor::HexRgb).toUpper() : QStringLiteral("#000000");
}

QString blendColor(const QColor &base, const QColor &accent, double accentRatio) {
    const double baseRatio = 1.0 - accentRatio;
    return QColor(static_cast<int>(base.red() * baseRatio + accent.red() * accentRatio + 0.5),
                  static_cast<int>(base.green() * baseRatio + accent.green() * accentRatio + 0.5),
                  static_cast<int>(base.blue() * baseRatio + accent.blue() * accentRatio + 0.5))
        .name(QColor::HexRgb)
        .toUpper();
}

}  // namespace

CommandCenterMainWindow::CommandCenterMainWindow(QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle(QString::fromLatin1(kAppDisplayName));

    const ConfigMap cfg = m_cfgMgr.load();
    const ConfigSection netCfg = cfg.value(QStringLiteral("Network"));
    const ConfigSection uiCfg = cfg.value(QStringLiteral("UI"));

    const QString host = netCfg.value(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    const int port = netCfg.value(QStringLiteral("port"), QStringLiteral("10086")).toInt();
    const int dataPort = netCfg.value(QStringLiteral("data_port"), QStringLiteral("5001")).toInt();

    m_networkState = new NetworkState(host, port, dataPort, this);
    m_channelState = new ChannelAddressState(this);
    m_sessionHub = new SessionHub(this);
    {
        // Seed the global TDM state before the pages bind to it, and persist
        // every later change (page toggles, replay restore) to [Session].
        const ConfigSection sessionCfg = cfg.value(QStringLiteral("Session"));
        m_sessionHub->tdmContext()->setState(
            sessionCfg.value(QStringLiteral("tdm_enabled"), QStringLiteral("0")).toInt() != 0,
            sessionCfg.value(QStringLiteral("tdm_phase"), QStringLiteral("0")).toInt() != 1);
        connect(m_sessionHub->tdmContext(), &TdmContext::changed, this,
                [this](bool enabled, bool pair02) {
            ConfigMap config = m_cfgMgr.load();
            ConfigSection &session = config[QStringLiteral("Session")];
            session[QStringLiteral("tdm_enabled")] =
                enabled ? QStringLiteral("1") : QStringLiteral("0");
            session[QStringLiteral("tdm_phase")] =
                pair02 ? QStringLiteral("0") : QStringLiteral("1");
            m_cfgMgr.save(config);
        });
    }
    m_manualHost = m_networkState->host();
    m_manualPort = m_networkState->port();
    m_manualDataPort = m_networkState->dataPort();

    // New v3 models
    m_statusModel = new SystemStatusModel(this);
    m_themeManager = new ThemeManager(this);
    m_updateClient = new UpdateClient(this);

    const int savedW = uiCfg.value(QStringLiteral("window_width"), QStringLiteral("1920")).toInt();
    const int savedH = uiCfg.value(QStringLiteral("window_height"), QStringLiteral("1100")).toInt();
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        const int w = qMax(1200, qMin(savedW, avail.width()));
        const int h = qMax(900, qMin(savedH, avail.height()));
        resize(w, h);
    } else {
        resize(qMax(1200, savedW), qMax(900, savedH));
    }

    buildUi();

    // Pre-warm the heavy pages once after first show so the first tab switch
    // doesn't hitch on initial layout/paint (a QStackedWidget renders a page
    // lazily only when first shown — the realtime page has 32 mini-plots).
    QTimer::singleShot(0, this, [this]() {
        for (QWidget *p : {static_cast<QWidget *>(m_pageRt),
                           static_cast<QWidget *>(m_pageAnalyzer),
                           static_cast<QWidget *>(m_pageSweep)}) {
            if (p) p->grab();
        }
    });

    m_statusModel->setHost(host);
    m_statusModel->setPort(port);
    m_statusModel->setDataPort(dataPort);
    connect(m_themeManager, &ThemeManager::themeChanged,
            this, &CommandCenterMainWindow::applyPaletteToPages);
    m_themeManager->apply(uiCfg.value(QStringLiteral("theme"), QStringLiteral("dark")));
    if (m_settingsPanel) {
        m_settingsPanel->setTheme(m_themeManager->currentTheme());
    }
}

void CommandCenterMainWindow::buildUi() {
    auto *root = new QWidget;
    setCentralWidget(root);

    auto *shell = new QVBoxLayout(root);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    // === Top Status Bar ===
    m_statusBar = new TopStatusBar(root);
    shell->addWidget(m_statusBar);

    m_sessionToolbar = new SessionToolbar(root);
    shell->addWidget(m_sessionToolbar);

    connect(m_statusModel, &SystemStatusModel::statusChanged,
            m_statusBar, &TopStatusBar::updateFromStatus);
    connect(m_statusModel, &SystemStatusModel::statusChanged,
            this, [this](const SystemStatus &status) {
        if (!m_sessionToolbar) return;
        if (m_settingsPanel) {
            m_settingsPanel->setLinkStates(status.controlLink,
                                           status.acquisition,
                                           status.controlDetail,
                                           status.acquisitionDetail);
        }
        switch (status.acquisition) {
        case AcquisitionState::Streaming:
            m_sessionToolbar->setLiveConnection(QStringLiteral("● 正在接收"), "ok");
            break;
        case AcquisitionState::Connecting:
            m_sessionToolbar->setLiveConnection(QStringLiteral("● 数据连接中…"), "warn");
            break;
        case AcquisitionState::Stalled:
            m_sessionToolbar->setLiveConnection(QStringLiteral("● 数据停滞"), "error");
            break;
        case AcquisitionState::Error:
            m_sessionToolbar->setLiveConnection(QStringLiteral("● 采集异常"), "error");
            break;
        default:
            m_sessionToolbar->setLiveConnection(QStringLiteral("采集待机"), "info");
            break;
        }
    });

    // Ingress rate readout next to the receive button. The timeout can fire
    // late when the GUI thread is busy (heavy plot pages), so divide by the
    // measured window instead of assuming exactly one second.
    auto *netRateTimer = new QTimer(this);
    netRateTimer->setInterval(1000);
    auto lastRxBytes = std::make_shared<qint64>(0);
    auto rateClock = std::make_shared<QElapsedTimer>();
    rateClock->start();
    connect(netRateTimer, &QTimer::timeout, this, [this, lastRxBytes, rateClock]() {
        if (!m_sessionHub || !m_sessionToolbar) return;
        const qint64 rx = m_sessionHub->statistics().receivedBytes;
        const qint64 elapsedMs = qMax<qint64>(1, rateClock->restart());
        const qint64 delta = rx - *lastRxBytes;
        *lastRxBytes = rx;
        if (m_sessionHub->state() == SessionHub::State::Live) {
            m_sessionToolbar->setNetworkRate(
                qMax<qint64>(0, delta * 1000 / elapsedMs));
        } else {
            m_sessionToolbar->setNetworkRate(-1);
        }
    });
    netRateTimer->start();

    // === Global raw-stream recording (V6) — independent of the shown page ===
    m_recTimer = new QTimer(this);
    m_recTimer->setInterval(500);
    auto setRecUi = [this](bool rec, const QString &text) {
        const QString line0 = text.split(QLatin1Char('\n')).value(0);
        if (m_statusBar) m_statusBar->setRecordingState(rec, line0);
        if (m_sessionToolbar) m_sessionToolbar->setRecording(rec, line0);
        if (m_recordingPanel) m_recordingPanel->setRecordingStatus(rec, text);
    };
    connect(m_recTimer, &QTimer::timeout, this, [this, setRecUi]() {
        if (!m_sessionHub || !m_sessionHub->isRecording()) return;
        const qint64 mb = m_sessionHub->recordedBytes() / (1024 * 1024);
        const qint64 secs = m_recElapsed.elapsed() / 1000;

        // Triggered capture with an auto-stop window: stop and re-arm so the
        // next threshold crossing starts a fresh capture.
        const double autoStop = m_recordingPanel->triggerAutoStopSeconds();
        if (m_triggeredRecordingActive && autoStop > 0.0 &&
            m_recElapsed.elapsed() >= static_cast<qint64>(autoStop * 1000.0)) {
            m_triggeredRecordingActive = false;
            m_sessionHub->stopRecording();
            m_recTimer->stop();
            setRecUi(false, QStringLiteral("触发录制段结束，等待下次触发"));
            return;
        }
        if (m_lastStorageCheckSec < 0 ||
            secs - m_lastStorageCheckSec >= 5) {
            m_lastStorageCheckSec = secs;
            const ConfigSection recording =
                m_cfgMgr.load().value(QStringLiteral("Recording"));
            const qint64 warningBytes =
                recording.value(QStringLiteral("warn_free_space_gb"),
                                QStringLiteral("20")).toLongLong() *
                1024LL * 1024LL * 1024LL;
            const qint64 freeBytes =
                SessionRecorder::freeSpaceBytes(m_recordingPanel->saveDir());
            if (warningBytes > 0 && freeBytes >= 0 &&
                freeBytes < warningBytes) {
                m_sessionHub->stopRecording();
                setRecUi(
                    false,
                    QStringLiteral("录制已停止: 磁盘剩余空间低于 %1 GB")
                        .arg(warningBytes / (1024LL * 1024LL * 1024LL)));
                return;
            }
        }
        setRecUi(true, QStringLiteral("REC %1:%2 / %3 MB\n%4")
                           .arg(secs / 60, 2, 10, QLatin1Char('0'))
                           .arg(secs % 60, 2, 10, QLatin1Char('0'))
                           .arg(mb)
                           .arg(m_sessionHub->recordingPath()));
    });
    auto beginRecording = [this, setRecUi](bool triggered) -> bool {
        const QString dir = m_recordingPanel->saveDir();
        const QString session = m_recordingPanel->sessionName();
        const qint64 split = m_recordingPanel->splitBytes();
        if (m_sessionHub->startRecording(dir, session, split)) {
            m_triggeredRecordingActive = triggered;
            return true;
        } else {
            m_triggeredRecordingActive = false;
            setRecUi(false, QStringLiteral("录制启动失败（检查保存目录）"));
            return false;
        }
    };
    // Threshold trigger fired on the acquisition thread -> start a capture.
    connect(m_sessionHub, &SessionHub::triggerFired, this, [this, beginRecording](quint64 epoch) {
        if (!m_sessionHub || !m_recordingPanel->triggerEnabled()) return;
        if (m_sessionHub->timelineEpoch() != epoch) return;
        if (m_sessionHub->state() != SessionHub::State::Live ||
            m_sessionHub->isReplaying()) return;
        if (m_sessionHub->isRecording() || m_pendingRecord) return;
        if (m_sessionHub->isConnected()) beginRecording(true);
    });

    // Commit / cancel a pending record based on whether the session connects.
    connect(m_sessionHub, &SessionHub::connectionStateChanged, this,
            [this, beginRecording, setRecUi](bool connected) {
        if (connected) {
            if (m_pendingRecord) {
                m_pendingRecord = false;
                beginRecording(false);
            }
        } else {
            if (m_pendingRecord) {
                m_pendingRecord = false;
                setRecUi(false, QStringLiteral("未连接，未录制"));
            }
            if (m_sessionHub->isRecording()) {
                m_triggeredRecordingActive = false;
                m_recTimer->stop();
                m_sessionHub->stopRecording();
                setRecUi(false, QStringLiteral("连接断开，录制已停止"));
            }
        }
    });
    connect(m_sessionHub,
            &SessionHub::recordingStateChanged,
            this,
            [this, setRecUi](bool recording, const QString &path) {
        if (recording) {
            m_recElapsed.restart();
            m_lastStorageCheckSec = -1;
            m_recTimer->start();
            setRecUi(true,
                     QStringLiteral("REC 00:00 / 0 MB\n%1").arg(path));
        } else {
            m_triggeredRecordingActive = false;
            m_recTimer->stop();
            setRecUi(false, QString());
        }
    });
    connect(m_sessionHub,
            &SessionHub::recordingError,
            this,
            [this, setRecUi](const QString &message) {
        m_pendingRecord = false;
        m_triggeredRecordingActive = false;
        m_recTimer->stop();
        setRecUi(false, QStringLiteral("录制错误: %1").arg(message));
    });

    // Settings panel (popup)
    m_settingsPanel = new SettingsPanel(this);

    m_settingsPanel->setEndpoint(m_networkState->host(), m_networkState->port(), m_networkState->dataPort());
    const ConfigMap settingsCfg = m_cfgMgr.load();
    const ConfigSection settingsUiCfg = settingsCfg.value(QStringLiteral("UI"));
    const ConfigSection localTestCfg = settingsCfg.value(QStringLiteral("LocalTest"));
    m_settingsPanel->setTheme(settingsUiCfg.value(QStringLiteral("theme"), QStringLiteral("dark")));
    m_settingsPanel->setManagedTestSource(
        localTestCfg.value(QStringLiteral("source"), QStringLiteral("external")).trimmed().toLower()
        == QStringLiteral("managed"));
    m_settingsPanel->setSpikeWaveform(
        localTestCfg.value(QStringLiteral("waveform"), QStringLiteral("sine")).trimmed().toLower()
        == QStringLiteral("spike"));
    m_settingsPanel->setLocalTestPorts(
        localTestCfg.value(QStringLiteral("control_port"), QStringLiteral("10086")).toInt(),
        localTestCfg.value(QStringLiteral("data_port"), QStringLiteral("10086")).toInt());
    connect(m_statusBar, &TopStatusBar::settingsRequested, this, [this]() {
        m_settingsPanel->move(m_statusBar->mapToGlobal(
            QPoint(m_statusBar->width() - m_settingsPanel->width() - 10, m_statusBar->height())));
        m_settingsPanel->show();
        if (!m_updateCheckedThisSession && m_updateClient) {
            m_updateCheckedThisSession = true;
            m_updateClient->checkForUpdates(appVersionDisplay());
        }
    });
    connect(m_settingsPanel, &SettingsPanel::updateCheckRequested, this, [this]() {
        if (m_updateClient) {
            m_updateCheckedThisSession = true;
            m_updateClient->checkForUpdates(appVersionDisplay());
        }
    });
    connect(m_updateClient, &UpdateClient::checkStarted,
            m_settingsPanel, &SettingsPanel::setUpdateChecking);
    connect(m_updateClient, &UpdateClient::checkFinished,
            m_settingsPanel, &SettingsPanel::setUpdateResult);
    connect(m_settingsPanel, &SettingsPanel::applied, this,
            [this](const QString &host, int spiPort, int dataPort, const QString &theme) {
        const bool localApply = m_localTestMode && host == QStringLiteral("127.0.0.1");
        const bool endpointChanged = m_networkState->host() != host
                                     || m_networkState->port() != spiPort
                                     || m_networkState->dataPort() != dataPort;
        if (m_localTestMode && !localApply) {
            m_localTestMode = false;
            if (m_settingsPanel) {
                m_settingsPanel->setLocalTestEnabled(false);
            }
        }
        setEndpoint(host, spiPort, dataPort, !localApply, !localApply);
        m_statusModel->setHost(host);
        m_statusModel->setPort(spiPort);
        m_statusModel->setDataPort(dataPort);
        if (endpointChanged) {
            m_statusModel->setControlLink(ControlLinkState::Unknown);
            m_statusModel->setAcquisition(AcquisitionState::Idle);
        }
        m_themeManager->apply(theme);
    });
    connect(m_settingsPanel, &SettingsPanel::themeSelected, this,
            [this](const QString &theme) {
        m_themeManager->apply(theme);
        saveUiToConfig();
    });
    connect(m_settingsPanel, &SettingsPanel::connectRequested, this,
            [this](const QString &host, int spiPort, int dataPort) {
        const bool isLocalTest = (host == QStringLiteral("127.0.0.1"));
        if (isLocalTest && m_managedLocalTest &&
            !m_dummyServer) {
            m_settingsPanel->setConnectionProbeResult(
                false, QStringLiteral("内置测试数据源未启动"));
            return;
        }
        setEndpoint(host, spiPort, dataPort, false, !isLocalTest);
        m_statusModel->setHost(host);
        m_statusModel->setPort(spiPort);
        m_statusModel->setDataPort(dataPort);
        if (!m_sessionHub) {
            m_settingsPanel->setConnectionProbeResult(false, QStringLiteral("采集会话未初始化"));
            return;
        }

        const SessionHub::State state = m_sessionHub->state();
        if (state == SessionHub::State::Live && m_sessionHub->isConnected()) {
            m_settingsPanel->setConnectionProbeResult(
                true, QStringLiteral("已连接 %1：控制 %2，数据 %3").arg(host).arg(spiPort).arg(dataPort));
            return;
        }
        if (state == SessionHub::State::ConnectingLive) {
            m_settingsPanel->setConnectionChecking();
            return;
        }

        m_pendingRecord = false;
        if (m_sessionHub->isRunning()) {
            m_sessionHub->stop();  // leave BIN replay before opening a live link
        }
        if (m_sessionToolbar) {
            m_sessionToolbar->setReplayLoaded(QString());
        }
        m_settingsPanel->setConnectionChecking();
        m_statusModel->setControlLink(ControlLinkState::Checking,
                                      QStringLiteral("正在检测控制端口"));
        m_statusModel->setAcquisition(AcquisitionState::Connecting,
                                      QStringLiteral("正在建立数据连接"));
        const double sampleRate = qMax(1.0, m_pageAnalyzer
            ? m_pageAnalyzer->configuredLiveSampleRate()
            : m_cfgMgr.load()
                .value(QStringLiteral("Signal"))
                .value(QStringLiteral("sampling_rate"), QStringLiteral("20000"))
                .toDouble());
        if (!m_sessionHub->start(host, spiPort, dataPort, QStringLiteral("ctre"), sampleRate)) {
            m_statusModel->setAcquisition(AcquisitionState::Error,
                                          QStringLiteral("实时连接启动失败"));
            m_settingsPanel->setConnectionProbeResult(false, QStringLiteral("实时连接启动失败"));
        }
    });
    connect(m_settingsPanel, &SettingsPanel::localTestToggled, this,
            [this](bool enabled, int spiPort, int dataPort, bool managedSource) {
        if (enabled) {
            const bool requestedSpike =
                managedSource && m_settingsPanel && m_settingsPanel->spikeWaveform();
            const bool endpointChanged = m_networkState->host() != QStringLiteral("127.0.0.1")
                                         || m_networkState->port() != spiPort
                                         || m_networkState->dataPort() != dataPort;
            if (!m_localTestMode) {
                m_manualHost = m_networkState->host();
                m_manualPort = m_networkState->port();
                m_manualDataPort = m_networkState->dataPort();
            } else if (!endpointChanged &&
                       managedSource == m_managedLocalTest &&
                       requestedSpike == m_managedDummySpike &&
                       (!managedSource ||
                        m_dummyServer)) {
                return;
            }
            m_localTestMode = true;
            m_managedLocalTest = managedSource;
            m_managedDummySpike = requestedSpike;
            stopManagedDummy();
            const bool sourceReady =
                !m_managedLocalTest || startManagedDummy(spiPort, dataPort);
            setEndpoint(QStringLiteral("127.0.0.1"), spiPort, dataPort, false, false);
            m_statusModel->setHost(m_networkState->host());
            m_statusModel->setPort(m_networkState->port());
            m_statusModel->setDataPort(m_networkState->dataPort());
            if (!sourceReady) {
                m_statusModel->setControlLink(ControlLinkState::Error,
                                              QStringLiteral("内置测试控制端口启动失败"));
                m_statusModel->setAcquisition(AcquisitionState::Error,
                                              QStringLiteral("内置测试数据源启动失败"));
            } else if (endpointChanged) {
                m_statusModel->setControlLink(ControlLinkState::Unknown);
                m_statusModel->setAcquisition(AcquisitionState::Idle);
            }
            saveLocalTestConfig(m_managedLocalTest, spiPort, dataPort);
        } else {
            if (!m_localTestMode) {
                return;
            }
            m_localTestMode = false;
            m_managedLocalTest = false;
            m_managedDummySpike = false;
            stopManagedDummy();
            const bool endpointChanged = m_networkState->host() != m_manualHost
                                         || m_networkState->port() != m_manualPort
                                         || m_networkState->dataPort() != m_manualDataPort;
            setEndpoint(m_manualHost, m_manualPort, m_manualDataPort, false, true);
            m_statusModel->setHost(m_networkState->host());
            m_statusModel->setPort(m_networkState->port());
            m_statusModel->setDataPort(m_networkState->dataPort());
            if (endpointChanged) {
                m_statusModel->setControlLink(ControlLinkState::Unknown);
                m_statusModel->setAcquisition(AcquisitionState::Idle);
            }
        }
    });

    auto *tabsFrame = new QFrame(root);
    tabsFrame->setObjectName(QStringLiteral("v5PageTabs"));
    auto *tabsLayout = new QHBoxLayout(tabsFrame);
    tabsLayout->setContentsMargins(12, 6, 12, 0);
    tabsLayout->setSpacing(6);

    auto *tabGroup = new QButtonGroup(tabsFrame);
    tabGroup->setExclusive(true);
    auto addTab = [tabsLayout, tabGroup](const QString &text, int index) {
        auto *btn = new QPushButton(text);
        btn->setObjectName(QStringLiteral("pageTab"));
        btn->setCheckable(true);
        tabGroup->addButton(btn, index);
        tabsLayout->addWidget(btn);
        return btn;
    };
    auto *mapTab = addTab(QStringLiteral("通道图"), 0);
    addTab(QStringLiteral("实时波形"), 1);
    addTab(QStringLiteral("分析器"), 2);
    addTab(QStringLiteral("实时spike"), 3);
    addTab(QStringLiteral("Spike留存"), 4);
    mapTab->setChecked(true);
    tabsLayout->addStretch(1);
    shell->addWidget(tabsFrame);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, root);
    mainSplitter->setChildrenCollapsible(false);
    // Page content contains dynamic status/detail labels. Their size hints must
    // never propagate to the top-level window and clear its maximized/fullscreen
    // state when text changes.
    mainSplitter->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mainSplitter->setMinimumSize(0, 0);
    shell->addWidget(mainSplitter, 1);

    auto *spiPane = new QFrame(mainSplitter);
    spiPane->setObjectName(QStringLiteral("persistentSpiPane"));
    spiPane->setMinimumWidth(280);
    spiPane->setMaximumWidth(360);
    auto *spiLayout = new QVBoxLayout(spiPane);
    spiLayout->setContentsMargins(0, 8, 0, 0);
    spiLayout->setSpacing(6);

    // Secondary switch: 硬件控制 / 采集存储
    auto *paneTabs = new QHBoxLayout;
    paneTabs->setContentsMargins(8, 0, 8, 0);
    paneTabs->setSpacing(6);
    auto *tabHw = new QPushButton(QStringLiteral("硬件控制"));
    auto *tabRec = new QPushButton(QStringLiteral("采集存储"));
    auto *paneGroup = new QButtonGroup(spiPane);
    paneGroup->setExclusive(true);
    int paneIdx = 0;
    for (auto *b : {tabHw, tabRec}) {
        b->setObjectName(QStringLiteral("paneSubTab"));
        b->setCheckable(true);
        b->setMinimumWidth(0);
        paneGroup->addButton(b, paneIdx++);
        paneTabs->addWidget(b, 1);
    }
    tabHw->setChecked(true);
    spiLayout->addLayout(paneTabs);

    auto *paneStack = new QStackedWidget;
    m_pageSpi = new SpiControlPanel(&m_cfgMgr, m_networkState, m_channelState, spiPane);
    m_recordingPanel = new RecordingPanel;
    paneStack->addWidget(m_pageSpi);
    paneStack->addWidget(m_recordingPanel);
    spiLayout->addWidget(paneStack, 1);
    connect(paneGroup, &QButtonGroup::idClicked, paneStack, &QStackedWidget::setCurrentIndex);

    // Storage settings live here; operational controls live only in the top toolbar.
    const ConfigSection recordingCfg =
        m_cfgMgr.load().value(QStringLiteral("Recording"));
    m_recordingPanel->setSaveDir(
        recordingCfg.value(QStringLiteral("save_dir"),
                           QStringLiteral("d:/ADC_data")));
    m_recordingPanel->setSessionName(
        recordingCfg.value(QStringLiteral("session_name")));
    m_recordingPanel->setSplitBytes(
        recordingCfg.value(QStringLiteral("split_bytes"),
                           QString::number(2LL << 30)).toLongLong());
    m_recordingPanel->setTriggerSettings(
        recordingCfg.value(QStringLiteral("trigger_enabled"), QStringLiteral("0")).toInt() != 0,
        recordingCfg.value(QStringLiteral("trigger_channel"), QStringLiteral("0")).toInt(),
        recordingCfg.value(QStringLiteral("trigger_threshold_mv"), QStringLiteral("900")).toDouble(),
        recordingCfg.value(QStringLiteral("trigger_rising"), QStringLiteral("1")).toInt() != 0,
        recordingCfg.value(QStringLiteral("trigger_autostop_sec"), QStringLiteral("0")).toDouble());
    connect(m_recordingPanel,
            &RecordingPanel::settingsChanged,
            this,
            [this]() {
        ConfigMap config = m_cfgMgr.load();
        ConfigSection &recording = config[QStringLiteral("Recording")];
        recording[QStringLiteral("save_dir")] = m_recordingPanel->saveDir();
        recording[QStringLiteral("session_name")] =
            m_recordingPanel->sessionName();
        recording[QStringLiteral("split_bytes")] =
            QString::number(m_recordingPanel->splitBytes());
        m_cfgMgr.save(config);
    });
    // Push trigger config to the hub and persist it whenever it changes.
    auto applyTriggerCfg = [this]() {
        if (!m_sessionHub) return;
        SessionHub::TriggerConfig cfg;
        cfg.enabled = m_recordingPanel->triggerEnabled();
        cfg.channel = m_recordingPanel->triggerChannel();
        cfg.thresholdV = m_recordingPanel->triggerThresholdVolts();
        cfg.risingAbove = m_recordingPanel->triggerRisingAbove();
        cfg.tdmSlot = m_recordingPanel->triggerTdmSlot();
        m_sessionHub->setTriggerConfig(cfg);

        ConfigMap config = m_cfgMgr.load();
        ConfigSection &recording = config[QStringLiteral("Recording")];
        recording[QStringLiteral("trigger_enabled")] = cfg.enabled ? QStringLiteral("1") : QStringLiteral("0");
        recording[QStringLiteral("trigger_channel")] = QString::number(cfg.channel);
        recording[QStringLiteral("trigger_threshold_mv")] =
            QString::number(m_recordingPanel->triggerThresholdVolts() * 1000.0);
        recording[QStringLiteral("trigger_rising")] = cfg.risingAbove ? QStringLiteral("1") : QStringLiteral("0");
        recording[QStringLiteral("trigger_autostop_sec")] =
            QString::number(m_recordingPanel->triggerAutoStopSeconds());
        // Persist the electrode select only when it's a real choice (TDM on).
        if (cfg.tdmSlot >= 0) {
            recording[QStringLiteral("trigger_tdm_slot")] = QString::number(cfg.tdmSlot);
        }
        m_cfgMgr.save(config);
    };
    connect(m_recordingPanel, &RecordingPanel::triggerSettingsChanged, this, applyTriggerCfg);
    // Show/hide the electrode selector with the global TDM state, and re-push
    // the trigger config so tdmSlot flips to/from -1 as the row appears/hides.
    if (m_sessionHub->tdmContext()) {
        m_recordingPanel->setTriggerTdmSlot(
            m_cfgMgr.load().value(QStringLiteral("Recording"))
                .value(QStringLiteral("trigger_tdm_slot"), QStringLiteral("0")).toInt());
        m_recordingPanel->setTdmMode(m_sessionHub->tdmContext()->enabled(),
                                     m_sessionHub->tdmContext()->pair02());
        connect(m_sessionHub->tdmContext(), &TdmContext::changed, this,
                [this, applyTriggerCfg](bool enabled, bool pair02) {
            m_recordingPanel->setTdmMode(enabled, pair02);
            applyTriggerCfg();
        });
    }
    applyTriggerCfg();  // seed the hub from the loaded settings

    m_stack = new QStackedWidget(mainSplitter);
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->setMinimumSize(0, 0);
    m_pageMap = new ChannelMapPanel(m_channelState, &m_cfgMgr, m_networkState);
    // Batch channel config on the map -> reuse the left SPI panel's send pipeline.
    connect(m_pageMap, &ChannelMapPanel::spiBatchRequested,
            m_pageSpi, &SpiControlPanel::enqueueExternalCommands);
    connect(m_pageSpi, &SpiControlPanel::spiCommandApplied,
            m_pageMap, &ChannelMapPanel::applySpiShadowCommand);
    connect(m_pageSpi, &SpiControlPanel::busyChanged,
            m_pageMap, &ChannelMapPanel::setSpiBusy);
    connect(m_pageSpi, &SpiControlPanel::controlLinkChecking, this, [this]() {
        if (m_statusModel) {
            m_statusModel->setControlLink(ControlLinkState::Checking,
                                          QStringLiteral("正在发送 SPI 命令"));
        }
    });
    connect(m_pageSpi, &SpiControlPanel::controlLinkResult, this,
            [this](bool reachable, bool degraded, const QString &message) {
        if (!m_statusModel) return;
        if (!reachable) {
            m_statusModel->setControlLink(ControlLinkState::Error, message);
        } else if (degraded) {
            m_statusModel->setControlLink(ControlLinkState::Degraded, message);
        } else {
            m_statusModel->setControlLink(ControlLinkState::Reachable, message);
        }
    });
    m_pageRt = new RealtimePlotPanel(&m_cfgMgr, m_networkState, m_channelState);
    m_pageRt->setSessionHub(m_sessionHub);
    m_pageAnalyzer = new AnalyzerPanel(&m_cfgMgr, m_networkState, m_channelState);
    m_pageAnalyzer->setSessionHub(m_sessionHub);
    m_pageSweep = new SweepPlotPanel(&m_cfgMgr, m_networkState);
    m_pageSweep->setSessionHub(m_sessionHub);
    m_pageSpike = new SpikePanel(&m_cfgMgr);
    m_pageSpike->setSessionHub(m_sessionHub);

    m_stack->addWidget(m_pageMap);
    m_stack->addWidget(m_pageRt);
    m_stack->addWidget(m_pageAnalyzer);
    m_stack->addWidget(m_pageSweep);
    m_stack->addWidget(m_pageSpike);

    mainSplitter->addWidget(spiPane);
    mainSplitter->addWidget(m_stack);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({320, 1300});

    // Timestamp continuity is now an always-on background monitor sharing the
    // session stream (see the TimestampMonitorWorker setup below) — no separate
    // connection, so it no longer tears down acquisition.
    // V6 (Option A): pages are pure views. The visible page auto-subscribes to
    // the running session; the others detach. The session itself runs from the
    // global toolbar, so switching pages never tears down acquisition/recording.
    auto activatePage = [this](int idx) {
        if (idx == 1 && m_pageRt) m_pageRt->onActivated();
        else if (idx == 2 && m_pageAnalyzer) m_pageAnalyzer->onActivated();
        else if (idx == 3 && m_pageSweep) m_pageSweep->onActivated();
        else if (idx == 4 && m_pageSpike) m_pageSpike->onActivated();
    };
    auto deactivatePage = [this](int idx) {
        if (idx == 1 && m_pageRt) m_pageRt->onDeactivated();
        else if (idx == 2 && m_pageAnalyzer) m_pageAnalyzer->onDeactivated();
        else if (idx == 3 && m_pageSweep) m_pageSweep->onDeactivated();
        else if (idx == 4 && m_pageSpike) m_pageSpike->onDeactivated();
    };
    connect(tabGroup, &QButtonGroup::idClicked, this, [this, activatePage, deactivatePage](int index) {
        const int prev = m_stack->currentIndex();
        if (prev != index) deactivatePage(prev);
        m_stack->setCurrentIndex(index);
        activatePage(index);
    });

    auto configuredSampleRate = [this]() {
        if (m_pageAnalyzer) return qMax(1.0, m_pageAnalyzer->configuredLiveSampleRate());
        return qMax(1.0,
                    m_cfgMgr.load()
                        .value(QStringLiteral("Signal"))
                        .value(QStringLiteral("sampling_rate"),
                               QStringLiteral("20000"))
                        .toDouble());
    };

    // === Global session toolbar wiring ===
    // The two actions ARE the modes: 实时接收 starts the live stream (ending
    // any replay); 打开BIN loads a playback file (ending live receive).
    connect(m_sessionToolbar, &SessionToolbar::runToggleRequested, this,
            [this, activatePage, deactivatePage, configuredSampleRate]() {
        if (!m_sessionHub) return;
        const auto st = m_sessionHub->state();
        const bool liveActive = st == SessionHub::State::Live ||
                                st == SessionHub::State::ConnectingLive;
        if (liveActive) {
            m_pendingRecord = false;
            deactivatePage(m_stack->currentIndex());
            m_sessionHub->stop();
            m_sessionToolbar->setRunning(false);
            if (m_statusModel) {
                m_statusModel->setAcquisition(AcquisitionState::Idle,
                                              QStringLiteral("用户停止采集"));
            }
            return;
        }
        // Idle or replay -> switch to live receive.
        m_pendingRecord = false;
        deactivatePage(m_stack->currentIndex());
        if (m_sessionHub->isRunning()) m_sessionHub->stop();  // ends replay
        m_sessionToolbar->setReplayLoaded(QString());
        if (m_statusModel) {
            m_statusModel->setControlLink(ControlLinkState::Checking,
                                          QStringLiteral("正在发送采集命令"));
            m_statusModel->setAcquisition(AcquisitionState::Connecting,
                                          QStringLiteral("正在建立数据连接"));
        }
        if (m_sessionHub->start(m_networkState->host(),
                                m_networkState->port(),
                                m_networkState->dataPort(),
                                QStringLiteral("ctre"),
                                configuredSampleRate())) {
            activatePage(m_stack->currentIndex());
        } else if (m_statusModel) {
            // start() refused (e.g. still stopping) — don't leave the pill
            // stuck on "连接中".
            m_statusModel->setAcquisition(AcquisitionState::Error,
                                          QStringLiteral("采集会话启动被拒绝"));
        }
    });
    connect(m_sessionToolbar, &SessionToolbar::replayLoadRequested, this,
            [this, activatePage, deactivatePage, configuredSampleRate](const QString &file) {
        if (!m_sessionHub) return;
        if (m_sessionHub->isRecording() || m_pendingRecord) {
            // Never let a file-open silently kill an active recording.
            return;
        }
        m_pendingRecord = false;
        deactivatePage(m_stack->currentIndex());
        if (m_sessionHub->startReplay(file, configuredSampleRate())) {
            m_sessionToolbar->setRunning(false);
            m_sessionToolbar->setReplayLoaded(file);
            activatePage(m_stack->currentIndex());
        } else {
            m_sessionToolbar->setReplayLoaded(QString());
        }
    });
    connect(m_sessionToolbar, &SessionToolbar::replayTogglePlayRequested, this, [this]() { if (m_sessionHub) m_sessionHub->replayTogglePlay(); });
    connect(m_sessionToolbar, &SessionToolbar::replayJumpStartRequested, this, [this]() { if (m_sessionHub) m_sessionHub->replayJumpStart(); });
    connect(m_sessionToolbar, &SessionToolbar::replayJumpEndRequested, this, [this]() { if (m_sessionHub) m_sessionHub->replayJumpEnd(); });
    connect(m_sessionToolbar, &SessionToolbar::replaySkipRequested, this, [this](double s) { if (m_sessionHub) m_sessionHub->replaySkip(s); });
    connect(m_sessionToolbar, &SessionToolbar::replaySeekRequested, this, [this](double f) { if (m_sessionHub) m_sessionHub->replaySeekFraction(f); });
    connect(m_sessionToolbar, &SessionToolbar::replayStopRequested, this, [this, deactivatePage]() {
        m_pendingRecord = false;
        deactivatePage(m_stack->currentIndex());
        if (m_sessionHub) m_sessionHub->stop();
        m_sessionToolbar->setReplayLoaded(QString());
    });
    connect(m_sessionToolbar, &SessionToolbar::recordToggleRequested, this,
            [this, beginRecording, setRecUi, configuredSampleRate]() {
        if (!m_sessionHub) return;
        if (m_sessionHub->isReplaying()) {
            setRecUi(false, QStringLiteral("BIN 回放模式不能录制"));
            return;
        }
        if (m_sessionHub->isRecording() || m_pendingRecord) {
            m_pendingRecord = false;
            m_triggeredRecordingActive = false;
            m_sessionHub->stopRecording();
            m_recTimer->stop();
            setRecUi(false, QString());
            return;
        }
        if (m_sessionHub->isConnected()) { beginRecording(false); return; }
        m_pendingRecord = true;
        setRecUi(false, QStringLiteral("连接中…"));
        if (!m_sessionHub->isRunning()) {
            if (!m_sessionHub->start(m_networkState->host(),
                                     m_networkState->port(),
                                     m_networkState->dataPort(),
                                     QStringLiteral("ctre"),
                                     configuredSampleRate())) {
                m_pendingRecord = false;
                setRecUi(false, QStringLiteral("采集连接启动失败，未录制"));
            }
        }
    });

    // Hub -> top-bar pill + auto-attach the visible page when the session runs.
    connect(m_sessionHub, &SessionHub::connectionStateChanged, this,
            [this, activatePage, deactivatePage](bool connected) {
        // Replay isn't a hardware link, so don't flip the run button or the
        // connection pill for it.
        const bool live = !m_sessionHub->isReplaying();
        if (live) m_sessionToolbar->setRunning(connected);
        if (live && m_statusModel) {
            m_statusModel->setAcquisition(
                connected ? AcquisitionState::Streaming : AcquisitionState::Idle,
                connected ? QStringLiteral("数据流正在接收")
                          : QStringLiteral("采集待机"));
        }
        if (connected) {
            activatePage(m_stack->currentIndex());
        } else {
            deactivatePage(m_stack->currentIndex());
        }
    });
    connect(m_sessionHub,
            &SessionHub::stateChanged,
            this,
            [this](SessionHub::State state) {
        if (!m_statusModel) return;
        if (state == SessionHub::State::ConnectingLive) {
            m_statusModel->setAcquisition(AcquisitionState::Connecting,
                                          QStringLiteral("正在建立数据连接"));
            if (m_settingsPanel) m_settingsPanel->setConnectionChecking();
        } else if (state == SessionHub::State::Live) {
            m_statusModel->setAcquisition(AcquisitionState::Streaming,
                                          QStringLiteral("数据流正在接收"));
            if (m_settingsPanel) {
                m_settingsPanel->setConnectionProbeResult(
                    true,
                    QStringLiteral("已连接 %1：控制 %2，数据 %3")
                        .arg(m_networkState->host())
                        .arg(m_networkState->port())
                        .arg(m_networkState->dataPort()));
            }
        } else if (state == SessionHub::State::Error) {
            const QString detail = QStringLiteral("数据连接失败或数据流中断");
            if (m_statusModel->snapshot().acquisition != AcquisitionState::Stalled) {
                m_statusModel->setAcquisition(AcquisitionState::Error, detail);
            }
            if (m_settingsPanel) {
                m_settingsPanel->setConnectionProbeResult(
                    false, QStringLiteral("连接失败，请检查主机和端口"));
            }
        }
        m_statusModel->setSamplingRate(
            m_sessionHub ? m_sessionHub->sampleRate() : 20000.0);
    });
    connect(m_sessionHub, &SessionHub::controlLinkResult, this,
            [this](bool reachable, const QString &message) {
        if (!m_statusModel) return;
        m_statusModel->setControlLink(
            reachable ? ControlLinkState::Reachable : ControlLinkState::Error,
            message);
    });
    connect(m_sessionHub, &SessionHub::connectionEvent, this,
            [this](const QString &message) {
        if (!m_statusModel || !m_sessionHub || m_sessionHub->isReplaying()) return;
        if (message.contains(QStringLiteral("stalled"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("No DMA data"), Qt::CaseInsensitive)) {
            m_statusModel->setAcquisition(AcquisitionState::Stalled, message);
        } else if (message.contains(QStringLiteral("data port"), Qt::CaseInsensitive) ||
                   message.contains(QStringLiteral("Data socket disconnected"),
                                    Qt::CaseInsensitive)) {
            m_statusModel->setAcquisition(AcquisitionState::Error, message);
        }
    });
    connect(m_sessionHub, &SessionHub::replayPosition, this, [this](qint64 cur, qint64 total) {
        m_sessionToolbar->setReplayPosition(
            cur, total, m_sessionHub ? m_sessionHub->sampleRate() : 20000.0);
    });
    connect(m_sessionHub, &SessionHub::replayPlayingChanged, m_sessionToolbar, &SessionToolbar::setReplayPlaying);
    connect(m_sessionHub, &SessionHub::replayFinished, this, [this]() { m_sessionToolbar->setReplayPlaying(false); });
    connect(m_sessionHub, &SessionHub::replayError, m_sessionToolbar, &SessionToolbar::setReplayError);

    // === Always-on timestamp continuity monitor ===
    // Subscribes to the shared stream (no separate connection), runs the cheap
    // channel-0 continuity check every frame and the 256-channel intra-frame
    // check on every 64th frame, and reports health to the top status bar.
    m_tsMonitorQueue = std::make_shared<ThreadSafeQueue<QByteArray>>(256);
    m_tsMonitorStop = std::make_unique<std::atomic_bool>(false);
    m_tsMonitor = new TimestampMonitorWorker(m_tsMonitorQueue, m_tsMonitorStop.get(),
                                             /*expectedStep=*/0, /*intraFrameStride=*/64, this);
    m_sessionHub->addSubscriber(m_tsMonitorQueue);
    connect(m_sessionHub, &SessionHub::timelineReset, this,
            [this](quint64, qint64) { if (m_tsMonitor) m_tsMonitor->requestReset(0); });
    connect(m_tsMonitor, &TimestampMonitorWorker::statsUpdated, this,
            [this](const TimestampContinuityStats &stats) {
        if (m_statusBar) {
            const bool active = m_sessionHub && m_sessionHub->isRunning();
            const bool tdmActive = m_sessionHub && m_sessionHub->tdmContext()
                                   && m_sessionHub->tdmContext()->enabled();
            m_statusBar->setTimestampHealth(stats, active, tdmActive);
        }
    });
    m_tsMonitor->start();
}

void CommandCenterMainWindow::onSettingsApplied(const QString &host, int port, int dataPort, const QString &theme)
{
    setEndpoint(host, port, dataPort, true, !m_localTestMode);
    m_statusModel->setHost(m_networkState->host());
    m_statusModel->setPort(m_networkState->port());
    m_statusModel->setDataPort(m_networkState->dataPort());
    m_themeManager->apply(theme);
    saveUiToConfig();
}

void CommandCenterMainWindow::applyPaletteToPages(const ThemePalette &palette) {
    QMap<QString, QString> mapPalette;
    mapPalette.insert(QStringLiteral("bg"), cssColor(palette.appBg));
    mapPalette.insert(QStringLiteral("electrode"), cssColor(palette.cardBgElevated));
    mapPalette.insert(QStringLiteral("outline"), cssColor(palette.cardBorder));
    mapPalette.insert(QStringLiteral("selected"), cssColor(palette.primary));
    mapPalette.insert(QStringLiteral("block_deep"), cssColor(palette.cardBg));
    mapPalette.insert(QStringLiteral("block_light"), blendColor(palette.cardBgElevated, palette.textPrimary, 0.06));
    mapPalette.insert(QStringLiteral("block_label_deep"), cssColor(palette.textPrimary));
    mapPalette.insert(QStringLiteral("block_label_light"), cssColor(palette.textSecondary));
    mapPalette.insert(QStringLiteral("boundary"), cssColor(palette.cardBorder));
    // Electrode inner core: a step above the electrode body toward the text
    // color; selected core tints toward the accent so it reads in every theme.
    mapPalette.insert(QStringLiteral("inner"),
                      blendColor(palette.cardBgElevated, palette.textPrimary, 0.18));
    mapPalette.insert(QStringLiteral("inner_selected"),
                      blendColor(palette.cardBgElevated, palette.primary, 0.45));
    if (m_pageMap) {
        m_pageMap->setMapTheme(mapPalette);
    }

    QMap<QString, QString> wavePalette;
    wavePalette.insert(QStringLiteral("bg"), cssColor(palette.cardBg));
    wavePalette.insert(QStringLiteral("plotBg"), cssColor(palette.plotBg));
    wavePalette.insert(QStringLiteral("title"), cssColor(palette.textSecondary));
    wavePalette.insert(QStringLiteral("grid"), cssColor(palette.plotGrid));
    wavePalette.insert(QStringLiteral("minorGrid"), blendColor(palette.plotBg, palette.plotGrid, 0.45));
    wavePalette.insert(QStringLiteral("wave"), cssColor(palette.plotWave));
    wavePalette.insert(QStringLiteral("axis"), cssColor(palette.plotAxis));
    wavePalette.insert(QStringLiteral("border"), cssColor(palette.cardBorder));
    if (m_pageRt) {
        m_pageRt->setWaveTheme(wavePalette);
    }
    if (m_pageAnalyzer) {
        m_pageAnalyzer->setPlotTheme(wavePalette);
    }
    if (m_pageSpike) {
        m_pageSpike->setWaveTheme(wavePalette);
    }
    if (m_pageSweep) {
        m_pageSweep->setWaveTheme(wavePalette);
    }
}

void CommandCenterMainWindow::saveUiToConfig() {
    ConfigMap cfg = m_cfgMgr.load();
    cfg[QStringLiteral("UI")][QStringLiteral("theme")] = m_themeManager->currentTheme();
    const QRect g = isMaximized() ? normalGeometry() : geometry();
    cfg[QStringLiteral("UI")][QStringLiteral("window_width")] = QString::number(qMax(1200, g.width()));
    cfg[QStringLiteral("UI")][QStringLiteral("window_height")] = QString::number(qMax(900, g.height()));
    m_cfgMgr.save(cfg);
}

void CommandCenterMainWindow::saveNetworkToConfig(const QString &host, int port, int dataPort) {
    ConfigMap cfg = m_cfgMgr.load();
    cfg[QStringLiteral("Network")][QStringLiteral("host")] = host;
    cfg[QStringLiteral("Network")][QStringLiteral("port")] = QString::number(port);
    cfg[QStringLiteral("Network")][QStringLiteral("data_port")] = QString::number(dataPort);
    m_cfgMgr.save(cfg);
}

void CommandCenterMainWindow::saveLocalTestConfig(bool managedSource,
                                                  int controlPort,
                                                  int dataPort)
{
    ConfigMap cfg = m_cfgMgr.load();
    ConfigSection &local = cfg[QStringLiteral("LocalTest")];
    local[QStringLiteral("source")] =
        managedSource ? QStringLiteral("managed") : QStringLiteral("external");
    local[QStringLiteral("waveform")] =
        (m_settingsPanel && m_settingsPanel->spikeWaveform()) ? QStringLiteral("spike")
                                                              : QStringLiteral("sine");
    local[QStringLiteral("control_port")] = QString::number(controlPort);
    local[QStringLiteral("data_port")] = QString::number(dataPort);
    m_cfgMgr.save(cfg);
}

bool CommandCenterMainWindow::startManagedDummy(int controlPort, int dataPort)
{
    stopManagedDummy();

    // The dummy generator lives on its own thread: its pacing QTimer would be
    // starved by heavy plot pages on the GUI thread, throttling the local
    // test stream (observed as "network rate drops on page 2/3").
    auto *thread = new QThread(this);
    thread->setObjectName(QStringLiteral("DummyStreamServer"));
    auto *server = new DummyStreamServer;
    server->setWaveform(m_managedDummySpike
                            ? DummyWaveform::Spike
                            : DummyWaveform::Sine);
    server->moveToThread(thread);
    connect(server, &DummyStreamServer::serverError, this,
            [this](const QString &) {
            if (m_statusModel) {
                m_statusModel->setControlLink(ControlLinkState::Error,
                                              QStringLiteral("内置测试控制端口异常"));
                m_statusModel->setAcquisition(AcquisitionState::Error,
                                              QStringLiteral("内置测试数据源异常"));
        }
    });
    thread->start();

    bool ok = false;
    QString error;
    QMetaObject::invokeMethod(server, [server, controlPort, dataPort, &ok, &error]() {
        ok = server->start(controlPort, dataPort, &error);
    }, Qt::BlockingQueuedConnection);

    if (!ok) {
        QMetaObject::invokeMethod(server, [server]() { delete server; },
                                  Qt::BlockingQueuedConnection);
        thread->quit();
        thread->wait();
        delete thread;
        return false;
    }

    m_dummyServer = server;
    m_dummyThread = thread;
    return true;
}

void CommandCenterMainWindow::stopManagedDummy()
{
    if (!m_dummyServer) {
        return;
    }
    DummyStreamServer *server = m_dummyServer;
    QThread *thread = m_dummyThread;
    m_dummyServer = nullptr;
    m_dummyThread = nullptr;
    // Sockets/timers belong to the server thread — tear down there.
    QMetaObject::invokeMethod(server, [server]() { delete server; },
                              Qt::BlockingQueuedConnection);
    if (thread) {
        thread->quit();
        thread->wait();
        delete thread;
    }
}

void CommandCenterMainWindow::setEndpoint(const QString &host, int port, int dataPort, bool saveCfg, bool rememberManual) {
    const QString normalizedRequestedHost =
        host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    const bool endpointChanged =
        normalizedRequestedHost != m_networkState->host() ||
        port != m_networkState->port() || dataPort != m_networkState->dataPort();
    if (endpointChanged && m_sessionHub && m_sessionHub->isRunning()) {
        m_pendingRecord = false;
        m_triggeredRecordingActive = false;
        m_sessionHub->stop();
        if (m_sessionToolbar) {
            m_sessionToolbar->setRunning(false);
            m_sessionToolbar->setReplayLoaded(QString());
        }
    }
    m_networkState->setEndpoint(host, port, dataPort);
    const QString normalizedHost = m_networkState->host();
    const int normalizedPort = m_networkState->port();
    const int normalizedDataPort = m_networkState->dataPort();
    if (m_statusModel) {
        m_statusModel->setHost(normalizedHost);
        m_statusModel->setPort(normalizedPort);
        m_statusModel->setDataPort(normalizedDataPort);
        if (endpointChanged) {
            m_statusModel->setControlLink(ControlLinkState::Unknown);
            m_statusModel->setAcquisition(AcquisitionState::Idle);
        }
    }
    if (m_settingsPanel) {
        m_settingsPanel->setEndpoint(normalizedHost, normalizedPort, normalizedDataPort);
    }
    if (rememberManual) {
        m_manualHost = normalizedHost;
        m_manualPort = normalizedPort;
        m_manualDataPort = normalizedDataPort;
    }
    if (saveCfg) {
        saveNetworkToConfig(normalizedHost, normalizedPort, normalizedDataPort);
    }
}

void CommandCenterMainWindow::closeEvent(QCloseEvent *event) {
    saveUiToConfig();
    if (m_tsMonitor) {
        if (m_tsMonitorStop) m_tsMonitorStop->store(true);
        if (m_tsMonitorQueue) m_tsMonitorQueue->wakeAll();
        if (!m_tsMonitor->wait(2000)) {
            // Never destroy a running QThread: its stop flag and queue are
            // members that die before the QObject children on teardown.
            m_tsMonitor->wait();
        }
    }
    if (m_pageSpi) m_pageSpi->flushPendingConfig();
    if (m_pageMap) m_pageMap->shutdown();
    if (m_pageRt) m_pageRt->shutdown();
    if (m_pageAnalyzer) m_pageAnalyzer->shutdown();
    if (m_pageSweep) m_pageSweep->shutdown();
    if (m_pageSpike) m_pageSpike->shutdown();
    if (m_sessionHub) m_sessionHub->stop();
    stopManagedDummy();
    event->accept();
}

}  // namespace ccv2
