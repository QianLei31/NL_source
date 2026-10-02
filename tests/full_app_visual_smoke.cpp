#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTableWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cmath>
#include <iostream>

#include "config/config_manager.h"
#include "core/constants.h"
#include "service/session_hub.h"
#include "theme/theme_manager.h"
#include "ui/command_center_main_window.h"
#include "ui/spike_panel.h"
#include "ui/widgets/session_toolbar.h"
#include "ui/widgets/spike_analysis_controls.h"
#include "ui/widgets/spike_archive_browser.h"
#include "ui/widgets/spike_rule_editor.h"

namespace {
void pump(int ms) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}
QByteArray fixture(int frames) {
    QByteArray data(frames * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < ccv2::kChannelsTotal; ++ch) {
            double v = 2048 + 0.6 * std::sin(f * 0.47 + ch);
            if (ch < 128) {
                const int period = 420 + (ch % 17) * 29;
                const int phase = (f + ch * 23) % period;
                const double x = phase - 160;
                const double amplitude = (9 + ch % 19) * ((f / period) % 2 ? 1.2 : 0.8);
                v -= amplitude * std::exp(-x * x / 10.0);
                v += amplitude * 0.35 * std::exp(-(x - 7) * (x - 7) / 20.0);
            }
            const quint32 raw = (quint32(f) << ccv2::kTimestampShift) | quint32(qBound(0, int(std::lround(v)), 4095));
            qToLittleEndian<quint32>(raw, reinterpret_cast<uchar *>(data.data() + f * ccv2::kFrameBytes + ch * ccv2::kBytesPerPoint));
        }
    }
    return data;
}
bool save(QWidget *widget, const QString &path) {
    pump(80);
    if (!widget || !widget->grab().save(path)) return false;
    std::cout << path.toStdString() << '\n';
    return true;
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("NeuralLabVisualQa"));
    QCoreApplication::setApplicationName(QStringLiteral("FullAppVisualSmoke"));
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", tmp.filePath(QStringLiteral("config")).toUtf8());
    const QString output = qEnvironmentVariable("NL_FULL_APP_SCREENSHOTS", tmp.filePath(QStringLiteral("screenshots")));
    QDir().mkpath(output);
    const QString binPath = tmp.filePath(QStringLiteral("synthetic-spike-replay.bin"));
    QFile bin(binPath);
    const QByteArray frames = fixture(18000);
    if (!bin.open(QIODevice::WriteOnly) || bin.write(frames) != frames.size()) return 2;
    bin.close();
    ccv2::ConfigManager cfg;
    ccv2::ConfigMap settings = cfg.load();
    settings[QStringLiteral("UI")][QStringLiteral("theme")] = QStringLiteral("dark");
    settings[QStringLiteral("Session")][QStringLiteral("tdm_enabled")] = QStringLiteral("0");
    auto &spike = settings[QStringLiteral("SpikePanel")];
    spike[QStringLiteral("threshold_units")] = QStringLiteral("input_referred");
    spike[QStringLiteral("input_gain")] = QStringLiteral("60");
    spike[QStringLiteral("threshold_mode")] = QStringLiteral("1");
    spike[QStringLiteral("threshold_value")] = QStringLiteral("30");
    spike[QStringLiteral("retain")] = QStringLiteral("200");
    spike[QStringLiteral("y_scale_uv")] = QStringLiteral("500");
    if (!cfg.save(settings) || cfg.load().value(QStringLiteral("SpikePanel")).value(QStringLiteral("threshold_mode")) != QStringLiteral("1")) return 11;

    ccv2::CommandCenterMainWindow window;
    window.resize(1600, 1000);
    window.show();
    pump(150);
    auto *hub = window.findChild<ccv2::SessionHub *>();
    auto *panel = window.findChild<ccv2::SpikePanel *>();
    auto *theme = window.findChild<ccv2::ThemeManager *>();
    auto *toolbar = window.findChild<ccv2::SessionToolbar *>();
    if (!hub || !panel || !theme || !toolbar || qApp->styleSheet().isEmpty()) return 3;
    auto *neuralTools = panel->findChild<ccv2::SpikeAnalysisControls *>();
    auto *archiveBrowser = panel->findChild<ccv2::SpikeArchiveBrowser *>();
    auto *ruleEditor = panel->findChild<ccv2::SpikeRuleEditor *>();
    auto *toolsToggle = panel->findChild<QPushButton *>(QStringLiteral("spikeArchiveTools"));
    if (!neuralTools || !archiveBrowser || !ruleEditor || !toolsToggle) return 24;
    const QString archivePath = tmp.filePath(QStringLiteral("real-replay-event-archive"));

    for (const char *name : {"sweepLegacyCommand", "sweepLegacyCommandLabel", "analyzerLegacyPauseCapture"}) {
        auto *control = window.findChild<QWidget *>(QString::fromLatin1(name));
        if (!control || !control->isHidden()) return 23;
    }
    bool navigated = false;
    for (auto *button : window.findChildren<QPushButton *>(QStringLiteral("pageTab"))) {
        if (button->text().contains(QStringLiteral("Spike"))) { button->click(); navigated = true; break; }
    }
    if (!navigated) return 4;
    // Use the actual toolbar's application wiring, including replay-mode
    // selection/file chip. Only the native file-picker is replaced by the
    // known generated path, never the controller or rendered widgets.
    toolbar->replayLoadRequested(binPath);
    if (!hub->isReplaying()) return 4;
    // Record through the actual panel controls before the real replay starts.
    // No synthetic event store or precomputed classifier output is injected.
    toolsToggle->setChecked(true); pump(50);
    auto *archiveDestination = neuralTools->findChild<QLineEdit *>(QStringLiteral("analysisArchiveDestination"));
    auto *archiveStart = neuralTools->findChild<QPushButton *>(QStringLiteral("analysisStartArchive"));
    if (!archiveDestination || !archiveStart) return 25;
    archiveDestination->setText(archivePath); archiveStart->click();
    if (panel->eventArchiveStatus().state != ccv2::SpikeArchiveState::Running) return 26;
    toolsToggle->setChecked(false);
    toolbar->replayTogglePlayRequested();
    QElapsedTimer timer; timer.start();
    while (hub->currentFrameIndex() < 18000 && timer.elapsed() < 10000) pump(20);
    if (hub->currentFrameIndex() < 18000) return 5;
    pump(400);
    timer.restart();
    while (!panel->eventArchiveStatus().writerFinished && timer.elapsed() < 10000) pump(20);
    if (!panel->eventArchiveStatus().writerFinished || !panel->eventArchiveStatus().finishVerified ||
        panel->eventArchiveStatus().writtenEvents == 0) return 27;
    // Capture real shell + actual constructed pages; no substitute stylesheet,
    // no injected data stores, and no license or application-policy changes.
    const QStringList themes = qEnvironmentVariable("NL_VISUAL_THEMES", QStringLiteral("dark,light,soft-dark,midnight-indigo")).split(',');
    QWidget *liveWorkspace = nullptr;
    for (const QString &name : themes) {
        // Keep a real modeless workspace open while the global theme changes.
        // Reopening alone would miss runtime propagation/background defects.
        if (liveWorkspace) liveWorkspace->show();
        theme->apply(name);
        window.resize(1600, 1000);
        if (!save(&window, QDir(output).filePath(QStringLiteral("full-app-%1.png").arg(name)))) return 6;
        if (auto *open = panel->findChild<QPushButton *>(QStringLiteral("spikeSortingWorkspace"))) {
            open->click(); pump(100);
            QWidget *workspace = nullptr;
            for (auto *candidate : panel->findChildren<QWidget *>()) {
                if (candidate->isWindow() && candidate->metaObject()->className() == QByteArray("ccv2::SpikeSortingWidget")) workspace = candidate;
            }
            if (!workspace) return 7;
            liveWorkspace = workspace;
            workspace->resize(1120, 850);
            pump(30);
            const auto rendered = workspace->grab().toImage();
            const QColor background = rendered.pixelColor(1, 1);
            if (background.alpha() != 255 || background.rgb() != theme->currentPalette().appBg.rgb()) return 17;
            auto *feature = workspace->findChild<QWidget *>(QStringLiteral("candidateFeaturePlot"));
            if (!feature || feature->grab().toImage().pixelColor(8, feature->height() - 8).rgb() !=
                theme->currentPalette().plotBg.rgb()) return 18;
            auto *assign = workspace->findChild<QPushButton *>(QStringLiteral("candidateAssign"));
            auto *select = workspace->findChild<QPushButton *>(QStringLiteral("candidateSelectAll"));
            auto *clear = workspace->findChild<QPushButton *>(QStringLiteral("candidateClearSelection"));
            // Optional clear is absent in the published preview1 comparison.
            if (assign && select && clear) {
                clear->click(); pump(20);
                if (assign->isEnabled()) return 19;
                const QColor disabled = assign->grab().toImage().pixelColor(assign->width() / 2, 5);
                select->click(); pump(20);
                if (!assign->isEnabled()) return 20;
                const QColor enabled = assign->grab().toImage().pixelColor(assign->width() / 2, 5);
                if (disabled == enabled) return 21;
                clear->click();
                if (auto *freeze = workspace->findChild<QCheckBox *>(QStringLiteral("candidateFreeze"))) freeze->setChecked(false);
            }
            if (!save(workspace, QDir(output).filePath(QStringLiteral("candidate-workspace-%1.png").arg(name)))) return 8;
            workspace->close(); open->click(); pump(20);
            if (!workspace->isVisible()) return 9;
            workspace->hide();
        }
    }
    theme->apply(QStringLiteral("dark"));
    if (liveWorkspace) {
        liveWorkspace->show();
        liveWorkspace->resize(1000, 700);
        if (!save(liveWorkspace, QDir(output).filePath(QStringLiteral("candidate-workspace-compact.png")))) return 12;
        if (liveWorkspace->width() > 1000 || liveWorkspace->height() > 700) return 13;
        liveWorkspace->hide();
    }
    window.resize(1280, 900);
    if (!save(&window, QDir(output).filePath(QStringLiteral("full-app-compact.png")))) return 10;
    if (auto *details = panel->findChild<QPushButton *>(QStringLiteral("spikeAnalysisDetails"))) {
        auto *contents = panel->findChild<QWidget *>(QStringLiteral("spikeAnalysisDetailsPanel"));
        auto *summary = panel->findChild<QLabel *>(QStringLiteral("spikeCoverageSummary"));
        if (!contents || !contents->isHidden() || !summary || summary->isHidden() ||
            !summary->text().contains(QStringLiteral("未验证"))) return 14;
        details->click(); pump(50);
        if (contents->isHidden()) return 15;
        details->click();
        if (!contents->isHidden()) return 16;
    }
    // Additive neural workflow captures: keep all baseline/candidate images
    // above. Drive the constructed shell's own controls and native modeless
    // children, substituting only known paths for file-picker interaction.
    toolsToggle->setChecked(true);
    if (auto *offlineToggle = neuralTools->findChild<QPushButton *>(QStringLiteral("analysisOfflineToggle"))) offlineToggle->setChecked(true);
    neuralTools->setOfflinePaths(binPath, tmp.filePath(QStringLiteral("offline-output-not-started")));
    neuralTools->openArchiveRequested(archivePath);
    timer.restart(); while (archiveBrowser->busy() && timer.elapsed() < 15000) pump(10);
    auto *archiveTable = archiveBrowser->findChild<QTableWidget *>(QStringLiteral("archiveEvents"));
    if (archiveBrowser->busy() || !archiveBrowser->isVisible() || !archiveTable || archiveTable->rowCount() == 0 ||
        archiveBrowser->selectedEventId() == 0) return 28;

    // A second real replay pass supplies a currently running detector context.
    // Pause well before EOF, edit a rule through numeric controls, resume until
    // the processing service acknowledges its actual immutable revision.
    toolbar->replayLoadRequested(binPath); pump(60);
    if (!hub->isReplaying() || !panel->analysisRunning()) return 29;
    toolbar->replayTogglePlayRequested(); timer.restart();
    while (hub->currentFrameIndex() < 6000 && timer.elapsed() < 10000) pump(10);
    if (hub->currentFrameIndex() >= 17500 || hub->currentFrameIndex() < 6000) return 30;
    toolbar->replayTogglePlayRequested(); pump(200);
    const auto captured = panel->analysisStore()->snapshotLane(0);
    if (captured.events.isEmpty()) return 31;
    auto *openRules = neuralTools->findChild<QPushButton *>(QStringLiteral("analysisRuleEditor"));
    if (!openRules) return 32;
    openRules->click(); pump(80);
    auto *ruleUnit = ruleEditor->findChild<QSpinBox *>(QStringLiteral("ruleUnit"));
    auto *timeFrom = ruleEditor->findChild<QDoubleSpinBox *>(QStringLiteral("ruleTimeFrom"));
    auto *timeTo = ruleEditor->findChild<QDoubleSpinBox *>(QStringLiteral("ruleTimeTo"));
    auto *voltageFrom = ruleEditor->findChild<QDoubleSpinBox *>(QStringLiteral("ruleVoltageFrom"));
    auto *voltageTo = ruleEditor->findChild<QDoubleSpinBox *>(QStringLiteral("ruleVoltageTo"));
    auto *addBox = ruleEditor->findChild<QPushButton *>(QStringLiteral("ruleAddBox"));
    auto *applyRule = ruleEditor->findChild<QPushButton *>(QStringLiteral("ruleApply"));
    if (!ruleEditor->isVisible() || !ruleUnit || !timeFrom || !timeTo || !voltageFrom || !voltageTo ||
        !addBox || !applyRule || !addBox->isEnabled()) return 33;
    ruleUnit->setValue(1); timeFrom->setValue(0.0); timeTo->setValue(0.20);
    voltageFrom->setValue(-160); voltageTo->setValue(-25); addBox->click();
    if (ruleEditor->definitions().isEmpty() || !applyRule->isEnabled()) return 34;
    applyRule->click(); toolbar->replayTogglePlayRequested(); timer.restart();
    while (hub->currentFrameIndex() < 12000 && timer.elapsed() < 10000) pump(10);
    if (hub->currentFrameIndex() >= 17500 || hub->currentFrameIndex() < 12000) return 35;
    toolbar->replayTogglePlayRequested(); pump(350);
    auto *captureRules = ruleEditor->findChild<QPushButton *>(QStringLiteral("ruleCapture"));
    if (!captureRules || ruleEditor->property("appliedRevision").toULongLong() == 0) return 36;
    captureRules->click(); pump(60);
    if (ruleEditor->definitions().isEmpty()) return 37;
    for (const QString &name : themes) {
        // Keep both real modeless windows alive while switching global theme.
        archiveBrowser->show(); ruleEditor->show(); theme->apply(name);
        window.resize(1600, 1000); archiveBrowser->resize(1100, 810); ruleEditor->resize(1050, 800);
        pump(30);
        // A service-acknowledged clean rule cannot be resubmitted until edited.
        // Its disabled primary action must not paint like the enabled archive
        // primary action under the compact-control variant selectors.
        const QColor disabledRule = applyRule->grab().toImage().pixelColor(applyRule->width() / 2, 5);
        const QColor enabledArchive = archiveStart->grab().toImage().pixelColor(archiveStart->width() / 2, 5);
        if (applyRule->isEnabled() || !archiveStart->isEnabled() || disabledRule == enabledArchive) return 41;
        if (!save(&window, QDir(output).filePath(QStringLiteral("neural-full-app-%1.png").arg(name))) ||
            !save(ruleEditor, QDir(output).filePath(QStringLiteral("neural-rule-editor-%1.png").arg(name))) ||
            !save(archiveBrowser, QDir(output).filePath(QStringLiteral("neural-archive-browser-%1.png").arg(name)))) return 38;
        for (auto *view : {static_cast<QWidget *>(archiveBrowser), static_cast<QWidget *>(ruleEditor)}) {
            const auto image = view->grab().toImage();
            if (image.isNull() || image.pixelColor(2, 2).alpha() != 255 ||
                image.pixelColor(2, 2).rgb() != theme->currentPalette().appBg.rgb()) return 39;
        }
    }
    theme->apply(QStringLiteral("light")); window.resize(1280, 900);
    if (!save(&window, QDir(output).filePath(QStringLiteral("neural-full-app-compact.png")))) return 40;
    archiveBrowser->close(); ruleEditor->close();
    window.close();
    std::cout << "full_app_visual_smoke ok; real replay archived " << panel->eventArchiveStatus().writtenEvents
              << " events and applied rule revision " << ruleEditor->property("appliedRevision").toULongLong() << "\n";
    return 0;
}
