#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <memory>

#include "config/config_manager.h"
#include "core/constants.h"
#include "core/tdm_context.h"
#include "io/session_manifest.h"
#include "service/session_hub.h"
#include "service/dummy_stream_server.h"
#include "theme/theme_manager.h"
#include "ui/command_center_main_window.h"
#include "ui/spike_panel.h"
#include "ui/widgets/recording_panel.h"
#include "ui/widgets/session_toolbar.h"
#include "ui/widgets/settings_panel.h"

using namespace ccv2;
namespace {
int failures = 0;
void check(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void pump(int ms) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 6000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) pump(10);
    return condition();
}
int freePort() { QTcpServer server; require(server.listen(QHostAddress::LocalHost, 0), "free port"); return server.serverPort(); }
QPushButton *button(QWidget *parent, const QString &part) {
    for (auto *b : parent->findChildren<QPushButton *>()) if (b->text().contains(part)) return b;
    throw std::runtime_error(("missing button " + part).toStdString());
}
QCheckBox *checkBox(QWidget *parent, const QString &part) {
    for (auto *b : parent->findChildren<QCheckBox *>()) if (b->text().contains(part)) return b;
    throw std::runtime_error(("missing checkbox " + part).toStdString());
}
bool hasText(QWidget *parent, const QString &part) {
    for (auto *label : parent->findChildren<QLabel *>()) if (label->text().contains(part)) return true;
    return false;
}
void fileDialogClick(QPushButton *button, const QString &path, bool directory = false, bool cancel = false) {
    std::cout << "DIALOG " << button->text().toStdString() << " cancel=" << cancel << " directory=" << directory << std::endl;
    bool seen = false;
    QPointer<QFileDialog> prepared;
    QElapsedTimer timeout; timeout.start();
    QTimer driver; driver.setInterval(10);
    QObject::connect(&driver, &QTimer::timeout, &driver, [&] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QFileDialog *>(w);
            if (!dialog || !dialog->isVisible()) continue;
            seen = true;
            if (cancel || timeout.elapsed() > 6000) dialog->reject();
            else if (prepared != dialog) {
                prepared = dialog;
                dialog->setDirectory(directory ? path : QFileInfo(path).absolutePath());
                dialog->selectFile(directory ? path : QFileInfo(path).fileName());
            } else {
                if (auto *name = dialog->findChild<QLineEdit *>("fileNameEdit"))
                    name->setText(directory ? path : QFileInfo(path).fileName());
                static_cast<QDialog *>(dialog)->accept();
            }
            return;
        }
    });
    driver.start();
    QTimer::singleShot(8000, &driver, [&] {
        for (auto *w : QApplication::topLevelWidgets()) {
            if (auto *box=qobject_cast<QMessageBox *>(w)) { std::cerr << "DIALOG WARNING: " << box->text().toStdString() << '\n'; box->reject(); }
            if (auto *d=qobject_cast<QFileDialog *>(w)) d->reject();
        }
    });
    button->click(); driver.stop();
    require(seen, "actual file dialog was presented");
    std::cout << "DIALOG closed " << button->text().toStdString() << std::endl;
}
struct CloseWindow { CommandCenterMainWindow &window; ~CloseWindow() { window.close(); } };
}

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    std::cout.setf(std::ios::unitbuf);
    app.setOrganizationName("NeuralLabDummyQa"); app.setApplicationName("SessionUiE2E");
    try {
        QTemporaryDir temporary; require(temporary.isValid(), "temporary workspace");
        qputenv("XDG_CONFIG_HOME", temporary.filePath("config").toUtf8());
        const QString captures = temporary.filePath("captures"); QDir().mkpath(captures);
        ConfigManager cfg; auto config = cfg.load();
        const int control = freePort(); int data = freePort(); while (data == control) data = freePort();
        config["Network"]["host"] = "127.0.0.1";
        config["Network"]["port"] = QString::number(freePort());
        config["Network"]["data_port"] = config["Network"]["port"];
        config["Recording"]["save_dir"] = captures;
        config["Recording"]["split_bytes"] = "0";
        config["Recording"]["warn_free_space_gb"] = "0";
        config["Recording"]["trigger_tdm_slot"] = "1";
        require(cfg.save(config), "isolated config saved");
        CommandCenterMainWindow window; CloseWindow close{window}; window.resize(1360, 940); window.show(); pump(100);
        auto *hub = window.findChild<SessionHub *>(); auto *settings = window.findChild<SettingsPanel *>();
        auto *recording = window.findChild<RecordingPanel *>(); auto *toolbar = window.findChild<SessionToolbar *>();
        auto *spikes = window.findChild<SpikePanel *>();
        require(hub && settings && recording && toolbar && spikes, "actual application components");
        hub->tdmContext()->setState(true, false);
        check(!recording->isVisible() && recording->triggerTdmSlot() == 1,
              "hidden storage pane preserves restored TDM trigger electrode slot");
        hub->tdmContext()->setEnabled(false);
        check(recording->triggerTdmSlot() == -1, "TDM off explicitly selects all source frames");
        if (app.arguments().contains("--trigger-ui-state-only")) {
            window.close();
            return failures ? 1 : 0;
        }
        settings->setLocalTestPorts(control, data); settings->setManagedTestSource(true); settings->setSpikeWaveform(false);
        auto *local = settings->findChild<QCheckBox *>("settingsLocalToggle");
        require(local, "local test checkbox"); local->setChecked(true);
        settings->show(); settings->findChild<QPushButton *>("settingsConnectButton")->click();
        require(waitFor([&] { return hub->isConnected() && hub->statistics().distributedFrames > 500; }), "managed Dummy connected through settings button");
        check(hub->sampleRate() == 20000, "GUI configured sample rate used");
        check(cfg.load()["LocalTest"]["source"] == "managed", "managed mode persisted");
        check(hasText(settings, "已连接") || hasText(settings, "接收"), "connection feedback visible");
        settings->findChild<QPushButton *>("settingsSaveButton")->click();
        check(!settings->isVisible(), "settings save closes popup");
        std::cout << "SUI01 PASS: actual settings managed dual-port sine connect/save\n";

        button(&window, "采集存储")->click();
        require(recording->isVisible(), "storage pane visible");
        fileDialogClick(button(recording, "浏览"), captures, true);
        check(recording->saveDir() == captures, "directory picker applies destination");
        fileDialogClick(button(recording, "浏览"), {}, true, true);
        check(recording->saveDir() == captures, "cancel directory picker preserves destination");
        const auto edits = recording->findChildren<QLineEdit *>();
        require(edits.size() >= 2, "recording field editors");
        QLineEdit *nameEdit = nullptr;
        for (auto *edit : edits) if (edit->placeholderText().contains("自动")) nameEdit = edit;
        require(nameEdit, "session name editor");
        nameEdit->setText("gui_manual"); nameEdit->editingFinished();
        QComboBox *split = nullptr;
        for (auto *combo : recording->findChildren<QComboBox *>()) if (combo->findText("不分片") >= 0) split = combo;
        require(split, "split mode combo");
        for (int i=0; i<split->count(); ++i) { split->setCurrentIndex(i); check(recording->splitBytes() == split->currentData().toLongLong(), "all split choices retain byte mapping"); }
        split->setCurrentIndex(0);
        hub->tdmContext()->setState(true, false);
        auto *record = toolbar->findChild<QPushButton *>("recordChip"); require(record, "record toolbar button");
        record->click(); require(waitFor([&] { return hub->isRecording() && hub->recordedBytes() > 500*1024; }), "toolbar manual recording started");
        const QString folder = hub->recordingPath();
        check(!nameEdit->isEnabled() && !split->isEnabled(), "record settings locked while recording");
        check(!button(toolbar, "打开文件")->isEnabled(), "replay load disabled while recording");
        const auto bytesBefore = hub->recordedBytes();
        for (auto *tab : window.findChildren<QPushButton *>("pageTab")) { tab->click(); pump(40); check(hub->isRecording(), "record persists across every page"); }
        check(hub->recordedBytes() > bytesBefore, "recorded stream grows across navigation");
        check(spikes->analysisRunning(), "spike processing alive after hidden-page navigation");
        record->click(); require(waitFor([&] { return !hub->isRecording(); }), "toolbar recording stopped");
        SessionManifestData manifest; QString error;
        require(SessionManifest::read(folder, &manifest, &error), "recorded manifest exists");
        check(manifest.complete && !manifest.active && manifest.totalFrames > 0, "manual UI recording finalized complete");
        check(!manifest.metadata.neuralAnalysis.isEmpty(), "UI analysis metadata recorded");
        check(manifest.metadata.tdmKnown && manifest.metadata.tdmEnabled && !manifest.metadata.tdmEvenFirst,
              "UI recording persists selected TDM pair 1/3");
        check(nameEdit->isEnabled() && split->isEnabled(), "settings unlocked after stop");
        const QString firstBin = QDir(folder).filePath(manifest.parts.first().fileName);
        hub->tdmContext()->setEnabled(false);
        std::cout << "SUI02 PASS: storage fields, four split selections, toolbar record, every page, metadata, stop; frames=" << manifest.totalFrames << '\n';

        button(toolbar, "停止接收")->click(); require(waitFor([&] { return !hub->isRunning(); }), "live toolbar stop");
        nameEdit->clear(); nameEdit->editingFinished();
        record->click(); require(waitFor([&] { return hub->isRecording() && hub->recordedBytes() > 128*1024; }), "record while idle auto-connects then starts recording");
        check(QRegularExpression(QStringLiteral("^v6_\\d{8}_\\d{6}(_\\d{3})?$")).match(QFileInfo(hub->recordingPath()).fileName()).hasMatch(),
              "empty session name generates timestamp folder");
        record->click(); button(toolbar, "停止接收")->click();
        require(waitFor([&] { return !hub->isRunning() && !hub->isRecording(); }), "pending recording cleanup");
        std::cout << "SUI03 PASS: idle record automatically connects and starts, stop/reconnect safe\n";

        button(toolbar, "回放")->click();
        qint64 replayPosition = -1, replayTotal = -1;
        const auto positionConnection = QObject::connect(hub, &SessionHub::replayPosition, &window,
            [&](qint64 position, qint64 total) { replayPosition = position; replayTotal = total; });
        fileDialogClick(button(toolbar, "打开文件"), firstBin);
        require(hub->isReplaying(), "actual file-picker replay load");
        check(!record->isEnabled(), "record disabled during replay");
        require(replayPosition == 0 && replayTotal == manifest.totalFrames && manifest.integrity.clean(),
                "GUI replay starts at recorded origin with exact frame count and clean source timeline");
        auto *play = toolbar->findChild<QPushButton *>("transportPlay"); require(play, "play control");
        const auto transport = [&](const QString &tooltip) {
            for (auto *b : toolbar->findChildren<QPushButton *>("transportBtn"))
                if (b->toolTip() == tooltip) return b;
            throw std::runtime_error("missing replay transport tooltip");
        };
        auto *home = transport(QStringLiteral("跳到开头"));
        auto *end = transport(QStringLiteral("跳到结尾"));
        auto *backward = transport(QStringLiteral("后退 2.5 秒"));
        auto *forward = transport(QStringLiteral("前进 2.5 秒"));
        auto *seek = toolbar->findChild<QSlider *>();
        require(seek && seek->isVisible() && seek->isEnabled(), "visible replay seek slider");
        auto replayQueue = std::make_shared<ThreadSafeQueue<QByteArray>>(64); hub->addSubscriber(replayQueue);
        const auto seekButton = [&](QPushButton *b, qint64 target) {
            const quint64 oldEpoch = hub->timelineEpoch(); b->click();
            require(waitFor([&] { return replayPosition == target && hub->timelineEpoch() > oldEpoch &&
                hub->currentFrameIndex() == manifest.metadata.frameOrigin + target; }),
                "actual replay transport reaches exact file and source-frame position");
            check(hub->state() == SessionHub::State::ReplayPaused && replayQueue->size() == 0,
                  "paused transport seek keeps paused state and clears stale queued frames");
        };
        seekButton(end, replayTotal);
        seekButton(backward, qMax<qint64>(0, replayTotal - qint64(2.5 * hub->sampleRate())));
        seekButton(forward, qMin<qint64>(replayTotal, replayPosition + qint64(2.5 * hub->sampleRate())));
        seekButton(home, 0);
        seekButton(backward, 0);
        QStyleOptionSlider sliderStyle; sliderStyle.initFrom(seek);
        sliderStyle.orientation = Qt::Horizontal; sliderStyle.minimum = seek->minimum(); sliderStyle.maximum = seek->maximum();
        sliderStyle.sliderPosition = seek->sliderPosition(); sliderStyle.sliderValue = seek->value();
        const QRect handle = seek->style()->subControlRect(QStyle::CC_Slider, &sliderStyle, QStyle::SC_SliderHandle, seek);
        const QRect groove = seek->style()->subControlRect(QStyle::CC_Slider, &sliderStyle, QStyle::SC_SliderGroove, seek);
        const QPoint begin = handle.center();
        const QPoint middle(groove.left() + handle.width()/2 +
            QStyle::sliderPositionFromValue(seek->minimum(), seek->maximum(), 500, groove.width()-handle.width()), begin.y());
        const auto sendMouse = [&](QEvent::Type type, const QPoint &at, Qt::MouseButton changed, Qt::MouseButtons held) {
            QMouseEvent event(type, at, seek->mapToGlobal(at), changed, held, Qt::NoModifier);
            QApplication::sendEvent(seek, &event);
        };
        const quint64 beforeSliderEpoch = hub->timelineEpoch();
        sendMouse(QEvent::MouseButtonPress, begin, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, middle, Qt::NoButton, Qt::LeftButton);
        const int chosenSliderValue = seek->value();
        require(chosenSliderValue > 450 && chosenSliderValue < 550, "real slider drag selects midpoint fraction");
        sendMouse(QEvent::MouseButtonRelease, middle, Qt::LeftButton, Qt::NoButton);
        const qint64 chosenFrame = qint64((chosenSliderValue / 1000.0) * replayTotal);
        require(waitFor([&] { return replayPosition == chosenFrame && hub->timelineEpoch() > beforeSliderEpoch &&
            hub->currentFrameIndex() == manifest.metadata.frameOrigin + chosenFrame; }),
            "actual slider release asynchronously seeks exact selected file and source coordinates");
        check(hub->state() == SessionHub::State::ReplayPaused && replayQueue->size() == 0,
              "slider seek is paused with no stale pre-seek queue");
        play->click(); QByteArray postSeek; StreamBlockInfo postSeekInfo;
        require(waitFor([&] { return !postSeek.isEmpty() || replayQueue->pop(postSeek, 0, nullptr, &postSeekInfo); }), "replay toolbar play delivers post-seek bytes");
        if (hub->state() == SessionHub::State::ReplayPlaying) play->click();
        QFile raw(firstBin); require(raw.open(QIODevice::ReadOnly) && raw.seek(chosenFrame*kFrameBytes), "open independent seek byte oracle");
        check(!postSeek.isEmpty() && postSeek == raw.read(postSeek.size()) && postSeekInfo.valid() &&
              postSeekInfo.firstFrame() == manifest.metadata.frameOrigin + chosenFrame,
              "GUI slider replay begins with exact recorded raw bytes and source-frame provenance");
        seekButton(home, 0);
        hub->removeSubscriber(replayQueue); QObject::disconnect(positionConnection);
        play->click(); require(waitFor([&] { return hub->currentFrameIndex() > manifest.metadata.frameOrigin; }), "replay toolbar play from restored start");
        toolbar->findChild<QPushButton *>("fileChipClose")->click();
        require(waitFor([&] { return !hub->isRunning(); }), "replay chip closes stream");
        fileDialogClick(button(toolbar, "打开文件"), {}, false, true);
        check(!hub->isRunning(), "cancel replay picker leaves session stopped");
        std::cout << "SUI04 PASS: native Qt open/play, jump-end/start, backward/forward bounds, real slider drag, exact source coordinates and raw bytes, unload/cancel/record guard\n";

        button(&window, "采集存储")->click();
        auto *load = button(recording, "加载 BIN"); auto *exportMat = button(recording, "导出 MATLAB");
        fileDialogClick(load, {}, false, true); check(!exportMat->isEnabled(), "cancel first export source leaves export disabled");
        fileDialogClick(load, firstBin); require(exportMat->isEnabled(), "BIN picker enables MATLAB export");
        check(hasText(recording, "session.json"), "export imports recording metadata");
        check(checkBox(recording, "TDM 电极")->isChecked(), "recorded TDM mode restored to exporter");
        QComboBox *exportPhases = nullptr;
        for (auto *combo : recording->findChildren<QComboBox *>())
            if (combo->findText(QStringLiteral("四相 0 / 1 / 2 / 3 全部导出")) >= 0) exportPhases = combo;
        check(exportPhases && exportPhases->currentIndex() == 0 && !exportPhases->currentText().isEmpty(),
              "pair1/3 metadata leaves all-four-phase export label valid");
        const QString output = temporary.filePath("matlab"); QDir().mkpath(output);
        fileDialogClick(exportMat, output, true, true);
        check(QDir(output).entryList(QDir::Files).isEmpty() && exportMat->isEnabled(), "cancel export destination writes nothing");
        fileDialogClick(exportMat, output, true);
        std::cout << "SUI05 waiting for asynchronous export\n";
        require(waitFor([&] { return exportMat->isEnabled() && hasText(recording, "导出完成"); }, 20000), "MATLAB export through actual UI completed");
        check(QDir(output).entryList(QDir::Files).size() >= 1025, "GUI exporter writes all1024 physical electrodes and loader");
        checkBox(recording, "TDM 电极")->setChecked(false);
        auto *raw32 = checkBox(recording, "保留原始32位");
        for (bool raw : {false, true}) {
            raw32->setChecked(raw);
            const QString rawOutput = temporary.filePath(raw ? "matlab_raw32" : "matlab_adc12"); QDir().mkpath(rawOutput);
            fileDialogClick(exportMat, rawOutput, true);
            const QString expectedName = raw ? "ch000_raw32le.bin" : "ch000_i32le.bin";
            require(waitFor([&] { return exportMat->isEnabled() && QFileInfo::exists(QDir(rawOutput).filePath(expectedName)) && hasText(recording, "导出完成"); }, 20000), "GUI raw export mode completed");
            check(QDir(rawOutput).entryList(QDir::Files).size() >= 257, "GUI raw mode exports all256 channels and loader");
        }
        std::cout << "SUI05 PASS: load/export dialogs cancel and accept, manifest settings, async MATLAB completion\n";

        // Actual controls, rather than setTriggerSettings(), exercise all
        // value-change/persistence connections as well as live trigger logic.
        QDoubleSpinBox *stopSeconds = nullptr;
        for (auto *spin : recording->findChildren<QDoubleSpinBox *>()) if (spin->suffix().contains("秒")) stopSeconds = spin;
        require(stopSeconds, "auto-stop control"); stopSeconds->setValue(1);
        check(cfg.load()["Recording"]["trigger_autostop_sec"].toDouble() == 1, "auto-stop change persisted without another trigger edit");
        auto *triggerChannel = recording->findChild<QSpinBox *>();
        QDoubleSpinBox *triggerThreshold = nullptr; QComboBox *triggerPolarity = nullptr, *triggerSlot = nullptr;
        for (auto *spin : recording->findChildren<QDoubleSpinBox *>()) if (spin->suffix().contains("mV")) triggerThreshold = spin;
        for (auto *combo : recording->findChildren<QComboBox *>()) {
            if (combo->findText(QStringLiteral("下降沿跌破")) >= 0) triggerPolarity = combo;
            if (combo->count() == 2 && combo->itemText(0).startsWith(QStringLiteral("相位"))) triggerSlot = combo;
        }
        require(triggerChannel && triggerThreshold && triggerPolarity && triggerSlot, "actual trigger numeric and selection editors");
        triggerChannel->setValue(5); triggerThreshold->setValue(1800); triggerPolarity->setCurrentIndex(1);
        hub->tdmContext()->setState(true, false);
        for (int slot : {0, 1}) {
            triggerSlot->setCurrentIndex(slot);
            check(recording->triggerTdmSlot() == slot && cfg.load()["Recording"]["trigger_tdm_slot"].toInt() == slot,
                  "both real TDM electrode choices persist and reach panel trigger config");
        }
        check(cfg.load()["Recording"]["trigger_channel"].toInt() == 5 &&
              cfg.load()["Recording"]["trigger_threshold_mv"].toDouble() == 1800 &&
              cfg.load()["Recording"]["trigger_rising"] == "0" && recording->triggerChannel() == 5 &&
              recording->triggerThresholdVolts() == 1.8 && !recording->triggerRisingAbove(),
              "channel, millivolt threshold and falling-edge GUI editors persist exact trigger configuration");
        nameEdit->setText("gui_trigger"); nameEdit->editingFinished();
        int starts=0, stops=0;
        QObject::connect(hub, &SessionHub::recordingStateChanged, &window, [&](bool active, const QString &) { if(active) ++starts; else ++stops; });
        checkBox(recording, "按阈值自动起录")->setChecked(true);
        button(toolbar, "实时")->click();
        button(toolbar, "开始接收")->click();
        require(waitFor([&] { return hub->isConnected() && hub->statistics().distributedFrames > 1600; }), "edited trigger receives real Dummy data");
        check(starts == 0 && !hub->isRecording(), "unreachable 1800 mV GUI threshold prevents recording despite enabled real acquisition");
        triggerThreshold->setValue(1050);
        check(cfg.load()["Recording"]["trigger_threshold_mv"].toDouble() == 1050 && recording->triggerThresholdVolts() == 1.05,
              "live threshold edit persists and converts millivolts exactly");
        require(waitFor([&] { return starts >= 2 && stops >= 1; }, 12000), "GUI trigger auto-stops and rearms into second capture");
        checkBox(recording, "按阈值自动起录")->setChecked(false);
        if (hub->isRecording()) record->click();
        button(toolbar, "停止接收")->click();
        check(starts >= 2 && stops >= 1, "trigger callbacks observed through mainwindow wiring");
        std::cout << "SUI06 PASS: actual channel/threshold/falling-edge/TDM slot editors, threshold suppression then live capture, persistence, auto-stop/rearm; starts=" << starts << " stops=" << stops << '\n';

        // Switching the managed waveform recreates only the simulated source.
        settings->show(); settings->setSpikeWaveform(true);
        settings->findChild<QPushButton *>("settingsConnectButton")->click();
        require(waitFor([&] { return hub->isConnected() && hub->statistics().distributedFrames > 1000; }), "managed spike profile connects");
        check(cfg.load()["LocalTest"]["waveform"] == "spike", "managed spike profile persisted");
        settings->findChild<QPushButton *>("settingsCloseButton")->click();
        check(!settings->isVisible(), "settings close dismisses without disrupting live stream");
        auto *theme = window.findChild<ThemeManager *>(); require(theme, "application theme manager");
        settings->show();
        const auto swatches = settings->findChildren<QPushButton *>("settingsThemeSwatch");
        check(swatches.size() == 4, "all four theme buttons reachable");
        for (auto *swatch : swatches) {
            const QString key = swatch->property("themeKey").toString(); swatch->click(); pump(20);
            check(theme->currentTheme() == key && cfg.load()["UI"]["theme"] == key, "theme button applies and persists exact selected palette");
        }
        const QString finalTheme = theme->currentTheme();
        const int refused = freePort();
        local->setChecked(false); settings->setEndpoint("127.0.0.1", refused, refused);
        settings->findChild<QPushButton *>("settingsSaveButton")->click();
        check(!hub->isRunning() && cfg.load()["Network"]["port"].toInt() == refused,
              "manual endpoint save stops old managed connection and persists endpoint");
        settings->show(); settings->findChild<QPushButton *>("settingsConnectButton")->click();
        require(waitFor([&] { return !hub->isConnected() && hasText(settings, "失败"); }, 8000), "refused manual connection reports GUI failure");
        local->setChecked(true); settings->setManagedTestSource(true); settings->setSpikeWaveform(true);
        settings->findChild<QPushButton *>("settingsConnectButton")->click();
        require(waitFor([&] { return hub->isConnected() && hub->statistics().distributedFrames > 500; }), "GUI retry recovers with managed Dummy");
        window.close();
        check(!hub->isRunning() && !spikes->analysisRunning(), "close stops acquisition and detector threads");
        std::cout << "SUI07 PASS: spike profile, theme buttons/persistence, manual endpoint refusal/recovery, active app shutdown\n";

        {
            CommandCenterMainWindow restored; CloseWindow restoredClose{restored}; restored.show(); pump(40);
            auto *restoredSettings = restored.findChild<SettingsPanel *>();
            auto *restoredRecording = restored.findChild<RecordingPanel *>();
            auto *restoredHub = restored.findChild<SessionHub *>();
            check(restored.findChild<ThemeManager *>()->currentTheme() == finalTheme, "reopened mainwindow restores chosen theme");
            check(restoredSettings->managedTestSource() && restoredSettings->spikeWaveform(), "reopened settings restore saved Dummy source profile");
            check(restoredSettings->spiPort() == refused && restoredSettings->dataPort() == refused, "local tests preserve remembered manual endpoint");
            check(restoredRecording->triggerAutoStopSeconds() == 1 && restoredRecording->saveDir() == captures && restoredRecording->splitBytes() == 0,
                  "reopened mainwindow restores recording/trigger settings");
            check(restoredRecording->triggerChannel() == 5 && restoredRecording->triggerThresholdVolts() == 1.05 &&
                  !restoredRecording->triggerRisingAbove() && cfg.load()["Recording"]["trigger_tdm_slot"] == "1",
                  "reopened mainwindow restores edited channel, threshold, polarity and selected TDM slot");
            check(!restoredHub->isRunning(), "reopening configuration never auto-starts acquisition");
            restoredSettings->setLocalTestPorts(control, data); restoredSettings->findChild<QCheckBox *>("settingsLocalToggle")->setChecked(true);
            restoredSettings->findChild<QPushButton *>("settingsSaveButton")->click();
            restoredRecording->setSessionName("close_while_recording");
            restored.findChild<SessionToolbar *>()->findChild<QPushButton *>("recordChip")->click();
            require(waitFor([&] { return restoredHub->isRecording() && restoredHub->recordedBytes() >= 128*1024; }), "reopened app records before close");
            const QString closingPath = restoredHub->recordingPath(); restored.close();
            SessionManifestData closedManifest;
            check(SessionManifest::read(closingPath, &closedManifest, &error) && !closedManifest.active && closedManifest.complete,
                  "close while recording safely finalizes complete raw session");
        }
        std::cout << "SUI08 PASS: full-window config reopen, remembered manual endpoint, close during recording\n";
        {
            auto exporting = std::make_unique<CommandCenterMainWindow>(); exporting->show(); pump(30);
            button(exporting.get(), "采集存储")->click();
            auto *panel = exporting->findChild<RecordingPanel *>();
            fileDialogClick(button(panel, "加载 BIN"), firstBin);
            const QString closingOutput = temporary.filePath("close_during_export"); QDir().mkpath(closingOutput);
            auto *exportButton = button(panel, "导出 MATLAB");
            fileDialogClick(exportButton, closingOutput, true);
            check(!exportButton->isEnabled(), "actual export exposes busy state before window close");
            QElapsedTimer closeTimer; closeTimer.start();
            exporting->close(); exporting.reset();
            check(closeTimer.elapsed() < 5000, "destroying window cancels/drains active export without hanging");
            const QDir outputFolder(closingOutput);
            if (outputFolder.exists("load_tdm_ele_channels.m"))
                check(outputFolder.entryList(QDir::Files).size() >= 1025, "completed export marker is never written for missing electrode outputs");
        }
        std::cout << "SUI09 PASS: close while asynchronous1024-electrode export is active\n";
        {
            auto safetyConfig = cfg.load();
            // Exercise the reserve-space guard without filling the disk or
            // altering actual filesystem capacity.
            safetyConfig["Recording"]["warn_free_space_gb"] = "1000000";
            require(cfg.save(safetyConfig), "reserve-space test configuration");
            CommandCenterMainWindow safety; CloseWindow safetyClose{safety}; safety.show(); pump(30);
            auto *sourceSettings = safety.findChild<SettingsPanel *>();
            auto *safetyHub = safety.findChild<SessionHub *>();
            auto *storage = safety.findChild<RecordingPanel *>();
            sourceSettings->setLocalTestPorts(control, data);
            sourceSettings->findChild<QCheckBox *>("settingsLocalToggle")->setChecked(true);
            sourceSettings->findChild<QPushButton *>("settingsConnectButton")->click();
            require(waitFor([&] { return safetyHub->isConnected(); }), "safety fixture connects real Dummy");
            button(&safety, "采集存储")->click();
            QFile blocker(temporary.filePath("not_a_directory")); require(blocker.open(QIODevice::WriteOnly), "invalid path fixture"); blocker.write("x"); blocker.close();
            storage->setSaveDir(blocker.fileName()+"/child");
            auto *recordButton = safety.findChild<SessionToolbar *>()->findChild<QPushButton *>("recordChip");
            recordButton->click();
            check(!safetyHub->isRecording() && hasText(storage, "失败"), "GUI invalid recording destination shows failure without active capture");
            storage->setSaveDir(captures); storage->setSessionName("reserve_guard");
            recordButton->click();
            require(waitFor([&] { return !safetyHub->isRecording() && hasText(storage, "磁盘剩余空间低于"); }, 5000),
                    "GUI valid retry records then reserve-space guard stops safely");
            SessionManifestData guardManifest;
            check(SessionManifest::read(safetyHub->recordingPath(), &guardManifest, &error) && !guardManifest.active && guardManifest.totalFrames > 0,
                  "storage guard finalizes actual captured frames");
            check(safetyHub->isConnected(), "storage guard stops recording but preserves live acquisition");
        }
        std::cout << "SUI10 PASS: invalid destination UI, immediate valid retry, configured free-space guard\n";
        {
            const int externalPort = freePort();
            DummyStreamServer external; QString externalError;
            require(external.start(externalPort, externalPort, &externalError), "independent external Dummy starts");
            CommandCenterMainWindow externalWindow; CloseWindow externalClose{externalWindow}; externalWindow.show(); pump(30);
            auto *externalSettings = externalWindow.findChild<SettingsPanel *>();
            auto *externalHub = externalWindow.findChild<SessionHub *>();
            externalSettings->setLocalTestPorts(externalPort, externalPort);
            externalSettings->setManagedTestSource(false);
            externalSettings->findChild<QCheckBox *>("settingsLocalToggle")->setChecked(true);
            externalSettings->findChild<QPushButton *>("settingsConnectButton")->click();
            require(waitFor([&] { return externalHub->isConnected() && externalHub->statistics().distributedFrames > 500; }),
                    "external Dummy dropdown connects to independently owned source");
            check(cfg.load()["LocalTest"]["source"] == "external" &&
                  externalWindow.findChild<QThread *>("DummyStreamServer") == nullptr,
                  "external selection persists and does not create a managed server thread");
            externalWindow.close();
            QTcpSocket probe; probe.connectToHost(QHostAddress::LocalHost, externalPort);
            check(waitFor([&] { return probe.state() == QAbstractSocket::ConnectedState; }),
                  "closing GUI does not destroy external Dummy listener");
            probe.abort();
        }
        std::cout << "SUI11 PASS: external-source dropdown and independently owned server lifecycle\n";
    } catch (const std::exception &error) { std::cerr << "ABORT: " << error.what() << '\n'; return 2; }
    if (failures) return 1;
    std::cout << "dummy_session_ui_e2e OK\n"; return 0;
}
