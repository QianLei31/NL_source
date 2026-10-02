#include "io/spike_event_archive.h"
#include "signal/spike_rule_classifier.h"
#include "theme/theme_manager.h"
#include "ui/widgets/spike_analysis_controls.h"
#include "ui/widgets/spike_archive_browser.h"
#include "ui/widgets/spike_rule_editor.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>

namespace {
int failures = 0, checks = 0;
void check(bool ok, const char *name) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
void idle(ccv2::SpikeArchiveBrowser &browser) {
    QElapsedTimer timer; timer.start();
    while (browser.busy() && timer.elapsed() < 15000) { QApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(1); }
    check(!browser.busy(), "asynchronous browser work finishes");
}
template<class T> T *child(QWidget &w, const char *name) { auto *value = w.findChild<T *>(QLatin1String(name)); check(value != nullptr, name); return value; }
// Exercise the production modal QFileDialog and its real Save/Open/Cancel
// buttons. The bounded timer runs in that dialog's nested Qt event loop; this
// does not replace QFileDialog functions or directly emit the caller's signal.
void fileDialogAction(QPushButton *launcher, const QString &selection, bool accept) {
    bool observed = false, submitted = false, cancelled = false, configured = false;
    int stableSelectionTicks = 0;
    QElapsedTimer deadline; deadline.start(); QTimer timer; timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (deadline.elapsed() > 5000) {
            check(false, "native Qt file dialog action completes within bound");
            if (dialog) {
                std::cerr << "Dialog timeout launcher=" << launcher->objectName().toStdString()
                          << " mode=" << int(dialog->fileMode()) << " accept=" << int(dialog->acceptMode())
                          << " directory=" << dialog->directory().absolutePath().toStdString()
                          << " selection=" << dialog->selectedFiles().join(';').toStdString() << '\n';
                if (auto *box = dialog->findChild<QDialogButtonBox *>()) for (auto *b : box->buttons())
                    std::cerr << "  button=" << b->text().toStdString() << " enabled=" << b->isEnabled() << " role=" << int(box->buttonRole(b)) << '\n';
                dialog->reject();
            }
            timer.stop(); return;
        }
        if (!dialog) return;
        observed = true;
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        if (!buttons) return;
        if (!accept) {
            if (auto *cancel = buttons->button(QDialogButtonBox::Cancel)) { cancelled = true; cancel->click(); }
            return;
        }
        if (!configured) {
            dialog->setDirectory(QFileInfo(selection).absolutePath());
            dialog->selectFile(selection); configured = true; return;
        }
        // QFileSystemModel loads/reset the selected directory asynchronously.
        // A one-time selectFile can be cleared by that reset, so verify the
        // actual selection and retry it until stable before pressing Open/Save.
        bool exactSelection = false;
        for (const auto &selected : dialog->selectedFiles())
            exactSelection = exactSelection || QDir::cleanPath(selected) == QDir::cleanPath(selection);
        if (!exactSelection) {
            stableSelectionTicks = 0;
            // selectFile alone may be ignored while the non-native dialog's
            // asynchronous file model refreshes. Enter the exact requested path
            // through the real filename editor, as a keyboard user would.
            if (auto *filename = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
                filename->setFocus();
                QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
                QKeyEvent typePath(QEvent::KeyPress, 0, Qt::NoModifier, selection);
                QApplication::sendEvent(filename, &selectAll); QApplication::sendEvent(filename, &typePath);
            } else dialog->selectFile(selection);
            return;
        }
        ++stableSelectionTicks;
        auto *commit = buttons->button(dialog->acceptMode() == QFileDialog::AcceptSave ? QDialogButtonBox::Save : QDialogButtonBox::Open);
        if (stableSelectionTicks >= 2 && commit && commit->isEnabled() && !submitted) { submitted = true; commit->click(); }
    });
    check(launcher && launcher->isEnabled(), "file dialog launcher enabled");
    if (!launcher || !launcher->isEnabled()) return;
    timer.start(); launcher->click(); timer.stop();
    check(observed, "actual QFileDialog displayed");
    check(accept ? submitted : cancelled, accept ? "actual file dialog acceptance button exercised" : "actual file dialog Cancel button exercised");
}

QMap<QString, QString> palette(const ccv2::ThemePalette &p) {
    return {{"appBg",p.appBg.name()},{"plotBg",p.plotBg.name()},{"border",p.cardBorder.name()},
        {"text",p.textPrimary.name()},{"title",p.textSecondary.name()},{"axis",p.plotAxis.name()},
        {"wave",p.plotWave.name()},{"accent",p.primary.name()},{"grid",p.plotGrid.name()}};
}
ccv2::SpikeArchiveRecord record(const QUuid &run, quint64 id, int electrode = 0) {
    ccv2::SpikeArchiveRecord r; auto &e = r.event;
    e.runId = run; e.eventId = id; e.epoch = 4; e.continuitySegment = 2; e.sequence = id;
    e.sourceFrame = 20000 + qint64(id) * 200; e.sourceSampleRate = 20000; e.inputGain = 1;
    e.lane = electrode; e.adcChannel = electrode; e.electrode = electrode; e.tdmPhase = -1; e.sampleStride = 1; e.preSamples = 8;
    e.unitId = 2; e.detectorRevision = 1; e.ruleRevision = 1; e.classification = ccv2::SpikeClassificationStatus::Assigned;
    for (int i = 0; i < 33; ++i) r.waveform.append(float((-95 * std::exp(-std::pow((i - 10) / 2.5, 2)) + 28 * std::exp(-std::pow((i - 18) / 4.2, 2))) * 1e-6));
    return r;
}
ccv2::SpikeRuleContext context(int electrode = 0) { ccv2::SpikeRuleContext c; c.adcChannel = c.electrode = electrode; return c; }
void render(QWidget &view, const QString &name, const QString &theme, const QColor &background) {
    view.show(); QApplication::processEvents(); const QImage image = view.grab().toImage();
    check(!image.isNull(), "real widget renders");
    check(image.pixelColor(2, 2).alpha() == 255, "modeless surface opaque");
    check(image.pixelColor(2, 2).rgb() == background.rgb(), "modeless surface matches theme");
    const QString output = qEnvironmentVariable("CCV2_ARCHIVE_RULE_QA_DIR");
    if (!output.isEmpty()) { QDir().mkpath(output); check(image.save(output + "/" + name + "-" + theme + ".png"), "real widget screenshot saved"); }
}
}

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); QTemporaryDir temporary; check(temporary.isValid(), "temporary directory");
    ccv2::ThemeManager theme; theme.apply(QStringLiteral("dark"));
    const QUuid run = QUuid::createUuid(); const QString path = temporary.path() + "/archive";
    ccv2::SpikeEventArchiveWriter writer; QString error;
    check(writer.begin(path, run, {{"fixture", "GUI synthetic waveform archive"}}, {}, &error), "begin sidecar fixture");
    check(writer.enqueueRevision(1, 1, {{"fixture", "detector and rule revision 1"}}) == ccv2::SpikeArchiveEnqueueResult::Accepted, "revision fixture accepted");
    QVector<ccv2::SpikeArchiveRecord> records;
    for (int i = 0; i < 270; ++i) records.append(record(run, quint64(i + 1), i % 2));
    check(writer.enqueueBatch(records, ccv2::SpikeArchiveEnqueueMode::OfflineBlocking) == ccv2::SpikeArchiveEnqueueResult::Accepted, "events fixture accepted");
    check(writer.finish(ccv2::SpikeArchiveState::Completed, {{"missing_source_frames", 2}, {"source_validity", "synthetic test"}}), "fixture drained"); writer.wait();
    check(writer.status().state == ccv2::SpikeArchiveState::Completed, "fixture completed");

    ccv2::SpikeArchiveBrowser browser;
    check(browser.openArchive(path), "open full archive request accepted"); idle(browser);
    auto *table = child<QTableWidget>(browser, "archiveEvents"); auto *next = child<QPushButton>(browser, "archiveNext"); auto *previous = child<QPushButton>(browser, "archivePrevious");
    auto *electrode = child<QSpinBox>(browser, "archiveElectrode"); auto *filter = child<QPushButton>(browser, "archiveFilter");
    auto *feedback = child<QLabel>(browser, "archiveFeedback");
    check(browser.visibleEventCount() == 128 && table->rowCount() == 128, "bounded first archive page");
    check(browser.selectedEventId() == 1 && next->isEnabled() && !previous->isEnabled(), "stable first event and cursor state");
    next->click(); idle(browser); check(browser.visibleEventCount() == 128 && browser.selectedEventId() == 129, "second cursor page exact stable identity");
    next->click(); idle(browser); check(browser.visibleEventCount() == 14 && browser.selectedEventId() == 257 && !next->isEnabled(), "final cursor page exact size");
    previous->click(); idle(browser); check(browser.selectedEventId() == 129, "previous cursor restores exact page");
    electrode->setValue(1); filter->click(); idle(browser); check(browser.visibleEventCount() == 128 && browser.selectedEventId() == 2, "physical electrode filter resets cursor");
    // Editing a filter does not retroactively change an already applied cursor.
    electrode->setValue(0); next->click(); idle(browser); check(browser.visibleEventCount() == 7 && browser.selectedEventId() == 258, "pending filter edits cannot mix page query");
    filter->click(); idle(browser); check(browser.selectedEventId() == 1, "new physical filter applies explicitly");
    auto *from = child<QDoubleSpinBox>(browser, "archiveTimeFrom"); auto *to = child<QDoubleSpinBox>(browser, "archiveTimeTo");
    from->setValue(1.05); to->setValue(1.10); filter->click(); idle(browser); check(browser.visibleEventCount() == 3 && browser.selectedEventId() == 5, "inclusive source time query");
    auto *epochOn = child<QCheckBox>(browser, "archiveEpochEnabled"); auto *epoch = child<QLineEdit>(browser, "archiveEpoch");
    epochOn->setChecked(true); epoch->setText(QStringLiteral("7")); filter->click(); idle(browser); check(browser.visibleEventCount() == 0, "epoch filter selects independent timeline");
    epoch->setText(QStringLiteral("4")); filter->click(); idle(browser); check(browser.selectedEventId() == 5, "known epoch returns stable identity");
    epoch->setText(QStringLiteral("18446744073709551616")); filter->click(); idle(browser); check(feedback->text().contains(QStringLiteral("64")), "overflow epoch rejected");
    epoch->setText(QStringLiteral("4")); filter->click(); idle(browser);
    const QString automated = table->item(0, 5)->text();
    auto *labelUnit = child<QSpinBox>(browser, "archiveLabelUnit"); auto *labelNote = child<QLineEdit>(browser, "archiveLabelNote"); auto *append = child<QPushButton>(browser, "archiveAppendLabel");
    labelUnit->setValue(7); labelNote->setText(QStringLiteral("人工检查 synthetic")); append->click(); idle(browser);
    check(feedback->text().contains(QStringLiteral("重新打开")), "UI annotation reports independent reopen proof");
    check(table->item(0, 5)->text() == automated, "manual annotation leaves automated candidate unchanged");
    ccv2::SpikeEventArchiveReader independentlyReopened; check(independentlyReopened.open(path, &error), "independent archive reopen");
    auto annotations = independentlyReopened.readAnnotations(5); check(annotations.annotations.size() == 1 && annotations.annotations[0].unitId == 7 && annotations.annotations[0].runId == run, "UI annotation persisted by stable identity");
    child<QPushButton>(browser, "archiveRemoveLabel")->click(); idle(browser);
    check(independentlyReopened.open(path, &error), "reopen label removal"); annotations = independentlyReopened.readAnnotations(5);
    check(annotations.annotations.size() == 2 && annotations.annotations.last().unitId == -1, "unlabel appends rather than deleting history");
    check(child<QLabel>(browser, "archiveDetails")->text().contains(QStringLiteral("missing_source_frames")), "source quality separately visible");
    browser.close(); browser.show(); app.processEvents(); check(browser.selectedEventId() == 5, "modeless archive reopen preserves page");
    check(browser.openArchive(temporary.path() + "/missing-archive"), "invalid path check is asynchronous"); idle(browser);
    check(browser.selectedEventId() == 5 && child<QLineEdit>(browser, "archivePath")->text() == path, "failed reopen preserves clearly identified last verified archive");
    auto *archiveBrowse = child<QPushButton>(browser, "archiveBrowse");
    fileDialogAction(archiveBrowse, path, true); idle(browser);
    check(browser.archiveDirectory() == path && browser.selectedEventId() == 5, "browser Browse accepts and verifies chosen archive");
    fileDialogAction(archiveBrowse, temporary.path(), false); idle(browser);
    check(browser.archiveDirectory() == path && browser.selectedEventId() == 5, "browser Browse cancel preserves current archive and selection");
    child<QPushButton>(browser, "archiveOpen")->click(); idle(browser);
    check(browser.visibleEventCount() == 3 && browser.selectedEventId() == 5, "browser Open revalidates and restores filtered event page");
    auto *archiveDetailsToggle = child<QPushButton>(browser, "archiveDetailsToggle");
    auto *archiveDetailsViewport = child<QWidget>(browser, "archiveDetailsViewport");
    check(archiveDetailsViewport->isHidden(), "archive details initially collapsed");
    archiveDetailsToggle->click(); app.processEvents();
    check(!archiveDetailsViewport->isHidden() && child<QLabel>(browser, "archiveDetails")->text().contains(QStringLiteral("missing_source_frames")), "archive details button exposes actual integrity metadata");
    archiveDetailsToggle->click(); check(archiveDetailsViewport->isHidden(), "archive details button collapses metadata");

    QVector<ccv2::SpikeArchiveAnnotation> history;
    for (int i = 0; i < 140; ++i) {
        ccv2::SpikeArchiveAnnotation annotation; annotation.runId = run; annotation.eventId = 5;
        annotation.unitId = 1 + i % 32; annotation.note = QStringLiteral("历史分页 %1").arg(i); history.append(annotation);
    }
    check(ccv2::SpikeEventArchiveReader::appendManualLabels(path, history, &error), "persist more than one real annotation page");
    child<QPushButton>(browser, "archiveOpen")->click(); idle(browser);
    auto *moreAnnotations = child<QPushButton>(browser, "archiveMoreAnnotations");
    auto *annotationHistory = child<QLabel>(browser, "archiveAnnotationHistory");
    check(moreAnnotations->isEnabled() && annotationHistory->text().contains(QStringLiteral("128")), "first bounded annotation page advertises continuation");
    moreAnnotations->click(); idle(browser);
    check(!moreAnnotations->isEnabled() && annotationHistory->text().contains(QStringLiteral("#142")) && annotationHistory->text().contains(QStringLiteral("历史分页 139")), "More Annotations button loads actual second page and exact final record");
    check(independentlyReopened.open(path, &error), "independently reopen paged annotation history");
    auto historyFirst = independentlyReopened.readAnnotations(5, 32, 128);
    auto historySecond = independentlyReopened.readAnnotations(5, historyFirst.nextOffset, 128);
    check(historyFirst.annotations.size() == 128 && historySecond.annotations.size() == 14 && historySecond.atEnd, "persisted annotation pagination independently verified 128 plus 14");
    // Select an actual visible table row with pointer events; a changed event
    // must update the selection and its annotation history rather than merely
    // relying on the automatic first-row selection performed during paging.
    browser.show(); table->scrollToItem(table->item(1, 0)); app.processEvents();
    const QPoint rowPoint = table->visualItemRect(table->item(1, 0)).center();
    check(table->viewport()->rect().contains(rowPoint), "archive row selection fixture visible");
    QMouseEvent rowPress(QEvent::MouseButtonPress, rowPoint, table->viewport()->mapToGlobal(rowPoint), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent rowRelease(QEvent::MouseButtonRelease, rowPoint, table->viewport()->mapToGlobal(rowPoint), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(table->viewport(), &rowPress); QApplication::sendEvent(table->viewport(), &rowRelease); idle(browser);
    check(browser.selectedEventId() == 7 && table->currentRow() == 1 && annotationHistory->text().contains(QStringLiteral("无人工标签")), "literal archive row click selects exact event and refreshes its history");




    ccv2::SpikeEventArchiveWriter liveWriter; const QString livePath = temporary.path() + "/unfinalized";
    check(liveWriter.begin(livePath, QUuid::createUuid(), {{"fixture", "unfinalized run"}}, {}, &error), "begin unfinalized fixture");
    ccv2::SpikeArchiveBrowser unfinishedBrowser; check(unfinishedBrowser.openArchive(livePath), "unfinalized archive can be inspected"); idle(unfinishedBrowser);
    check(child<QLabel>(unfinishedBrowser, "archiveStatus")->text().contains(QStringLiteral("无已验证终态")), "unfinalized archive never called completed");
    check(!child<QPushButton>(unfinishedBrowser, "archiveAppendLabel")->isEnabled(), "unfinalized archive disallows post-close annotation");
    check(liveWriter.finish(ccv2::SpikeArchiveState::Drained), "drained fixture finishes"); liveWriter.wait();
    check(unfinishedBrowser.openArchive(livePath), "reopen drained archive"); idle(unfinishedBrowser);
    check(child<QLabel>(unfinishedBrowser, "archiveStatus")->text().contains(QStringLiteral("停止并排空")), "drained lifecycle distinct from complete");
    unfinishedBrowser.close();

    // A pending worker never owns widget memory. Cancellation and destruction
    // return immediately; stale generations cannot repopulate a closed window.
    ccv2::SpikeArchiveBrowser cancelledBrowser; int cancelledOpens = 0;
    QObject::connect(&cancelledBrowser, &ccv2::SpikeArchiveBrowser::archiveOpened, [&](bool) { ++cancelledOpens; });
    check(cancelledBrowser.openArchive(path) && cancelledBrowser.busy(), "async open exposes busy immediately");
    child<QPushButton>(cancelledBrowser, "archiveCancelRead")->click(); check(!cancelledBrowser.busy(), "actual Cancel Read button cancels immediately");
    QApplication::processEvents(); check(cancelledOpens == 0, "cancelled result generation ignored");
    check(cancelledBrowser.openArchive(path), "new generation accepted after cancel"); idle(cancelledBrowser);
    check(cancelledBrowser.visibleEventCount() == 128 && cancelledOpens == 1, "new generation verifies and replaces view");
    auto *destroyedBrowser = new ccv2::SpikeArchiveBrowser;
    destroyedBrowser->openArchive(path); QElapsedTimer destroyTimer; destroyTimer.start(); delete destroyedBrowser;
    check(destroyTimer.elapsed() < 500, "browser destruction never waits for worker verification");
    cancelledBrowser.close();

    ccv2::SpikeRuleEditor editor; int captureRequests = 0, applyRequests = 0;
    QObject::connect(&editor, &ccv2::SpikeRuleEditor::captureRequested, [&](int lane) { ++captureRequests; check(lane == editor.lane(), "capture request lane exact"); });
    QObject::connect(&editor, &ccv2::SpikeRuleEditor::applyRulesRequested, [&](const QVector<ccv2::SpikeElectrodeRules> &definitions) {
        ++applyRequests; editor.setAppliedRules(ccv2::SpikeRuleSet::create(7, definitions), QStringLiteral("实际版本7，自下一交越起"));
    });
    check(captureRequests == 0 && applyRequests == 0, "opening editor causes no acquisition or rule application");
    child<QPushButton>(editor, "ruleCapture")->click(); check(captureRequests == 1, "capture button sends read request only");
    QVector<ccv2::SpikeArchiveRecord> capture; for (int i = 0; i < 6; ++i) capture.append(record(run, quint64(i + 1)));
    editor.setCapturedContext(context(), capture);
    auto *add = child<QPushButton>(editor, "ruleAddBox"); auto *boxes = child<QListWidget>(editor, "ruleBoxList"); auto *apply = child<QPushButton>(editor, "ruleApply");
    auto *unit = child<QSpinBox>(editor, "ruleUnit");
    auto *laneSpin = child<QSpinBox>(editor, "ruleLane");
    laneSpin->setValue(1);
    check(editor.lane() == 1 && captureRequests == 2 && !add->isEnabled() && applyRequests == 0,
          "literal rule lane edit requests lane1 and invalidates prior context without applying");
    laneSpin->setValue(0);
    check(editor.lane() == 0 && captureRequests == 3 && !add->isEnabled() && applyRequests == 0,
          "literal rule lane return requests lane0 and requires a new context capture");
    editor.setCapturedContext(context(), capture);
    check(add->isEnabled() && captureRequests == 3 && applyRequests == 0,
          "fresh requested lane context restores editing without implicit rule application");
    child<QDoubleSpinBox>(editor, "ruleTimeFrom")->setValue(0.05); child<QDoubleSpinBox>(editor, "ruleTimeTo")->setValue(0.15);
    child<QDoubleSpinBox>(editor, "ruleVoltageFrom")->setValue(-110); child<QDoubleSpinBox>(editor, "ruleVoltageTo")->setValue(-60);
    add->click(); check(boxes->count() == 1 && apply->isEnabled() && applyRequests == 0, "numeric box edit remains unapplied draft");
    auto *preview = child<QLabel>(editor, "rulePreview"); check(preview->text().contains(QStringLiteral("匹配 6")), "captured preview uses core classification");
    unit->setValue(2); add->click(); check(preview->text().contains(QStringLiteral("歧义 6")), "overlapping candidates explicitly ambiguous");
    boxes->setCurrentRow(1); child<QPushButton>(editor, "ruleDeleteBox")->click(); check(boxes->count() == 1 && preview->text().contains(QStringLiteral("匹配 6")), "deleting last box removes candidate cleanly");
    const QString ruleFile = temporary.path() + "/rules.json"; check(editor.saveRules(ruleFile), "draft save");
    editor.show(); app.processEvents();
    auto *ruleDetailsToggle = child<QPushButton>(editor, "ruleDetailsToggle");
    auto *ruleDetailsViewport = child<QWidget>(editor, "ruleDetailsViewport");
    check(ruleDetailsViewport->isHidden(), "rule details initially collapsed");
    ruleDetailsToggle->click(); app.processEvents();
    check(!ruleDetailsViewport->isHidden() && child<QLabel>(editor, "ruleDetails")->text().contains(QStringLiteral("AND")), "rule details button exposes matching and boundary explanation");
    ruleDetailsToggle->click(); check(ruleDetailsViewport->isHidden(), "rule details button collapses explanation");
    const QString dialogRuleFile = temporary.path() + "/dialog-rules.json";
    fileDialogAction(child<QPushButton>(editor, "ruleSave"), dialogRuleFile, true);
    const auto dialogSavedRules = ccv2::SpikeRuleSet::load(dialogRuleFile, &error);
    check(dialogSavedRules && dialogSavedRules->electrodes().size() == 1, "Save dialog writes real parseable rule definitions");
    const QString cancelledRuleFile = temporary.path() + "/cancelled-rules.json";
    fileDialogAction(child<QPushButton>(editor, "ruleSave"), cancelledRuleFile, false);
    check(!QFile::exists(cancelledRuleFile), "Save dialog cancellation creates no rule file");
    fileDialogAction(child<QPushButton>(editor, "ruleLoad"), dialogRuleFile, true);
    check(editor.definitions().size() == 1 && boxes->count() == 1, "Load dialog restores saved definitions as a draft");
    const QByteArray draftBeforeCancel = ccv2::SpikeRuleSet::create(1, editor.definitions())->toJson();
    fileDialogAction(child<QPushButton>(editor, "ruleLoad"), dialogRuleFile, false);
    check(ccv2::SpikeRuleSet::create(1, editor.definitions())->toJson() == draftBeforeCancel, "Load dialog cancellation preserves current draft");

    apply->click(); check(applyRequests == 1 && !apply->isEnabled(), "service acknowledges immutable actual revision");
    check(child<QLabel>(editor, "ruleRevision")->text().contains(QStringLiteral("7")), "actual service revision displayed");
    const auto desiredWhileStopped = ccv2::SpikeRuleSet::create(7, editor.definitions());
    editor.setAppliedRules({}, QStringLiteral("检测已停止")); editor.setRequestedRules(desiredWhileStopped);
    editor.close(); editor.show(); app.processEvents();
    check(!editor.definitions().isEmpty() && !apply->isEnabled(), "stopped reopen preserves saved desired rules without claiming application");
    check(child<QLabel>(editor, "ruleRevision")->text().contains(QStringLiteral("待应用版本 7")), "desired revision is explicitly pending while stopped");
    const QString stoppedFile = temporary.path() + "/stopped-rules.json";
    check(editor.saveRules(stoppedFile) && !ccv2::SpikeRuleSet::load(stoppedFile)->electrodes().isEmpty(), "stopped desired rules remain saveable and nonempty");
    editor.setAppliedRules(desiredWhileStopped); editor.setRequestedRules(desiredWhileStopped);
    check(editor.loadRules(ruleFile) && apply->isEnabled(), "load is explicit unapplied draft");
    editor.setRequestedRules(ccv2::SpikeRuleSet::create(8, {}));
    check(!editor.definitions().isEmpty() && apply->isEnabled(), "requested service update preserves dirty local draft");
    editor.setRequestedRules(desiredWhileStopped);
    auto incompatibleContext = context(); incompatibleContext.inputGain = 4;
    editor.setCapturedContext(incompatibleContext, capture);
    check(!add->isEnabled() && preview->text().contains(QStringLiteral("不兼容 6")), "changed gain cannot silently rebind rules");
    child<QPushButton>(editor, "ruleBindContext")->click(); check(add->isEnabled(), "explicit binding enables editing new context");
    check(!editor.addBox(1, {1, 1, -100, 100}), "zero-area rectangle rejected");
    editor.setCapturedContext(context(), capture); child<QPushButton>(editor, "ruleBindContext")->click();
    editor.show(); app.processEvents(); auto *plot = child<QWidget>(editor, "ruleWaveformPlot");
    const QPointF begin(95, 80), end(140, 110);
    QMouseEvent press(QEvent::MouseButtonPress, begin, begin, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(plot, &press); QApplication::sendEvent(plot, &release);
    check(boxes->count() == 2, "actual plot drag produces canonical box definition");
    editor.close(); editor.show(); app.processEvents(); check(boxes->count() == 2, "modeless editor reopen preserves draft");
    // A capture replacement cancels a pending drag rather than committing stale
    // pixel coordinates against an unrelated context.
    QApplication::sendEvent(plot, &press); editor.setCapturedContext(context(), capture); QApplication::sendEvent(plot, &release);
    check(boxes->count() == 2, "new context capture cancels stale mouse drag");

    ccv2::SpikeAnalysisControls controls; int starts = 0, stops = 0, analyses = 0, cancels = 0, launches = 0;
    QObject::connect(&controls, &ccv2::SpikeAnalysisControls::startArchiveRequested, [&](const QString &d) { ++starts; check(d == "/tmp/new-sidecar", "archive request destination exact"); });
    QObject::connect(&controls, &ccv2::SpikeAnalysisControls::stopArchiveRequested, [&]() { ++stops; });
    QObject::connect(&controls, &ccv2::SpikeAnalysisControls::analyzeRecordingRequested, [&](const QString &s, const QString &d) { ++analyses; check(s == "/tmp/recording" && d == "/tmp/offline-output", "offline source/output exact"); });
    QObject::connect(&controls, &ccv2::SpikeAnalysisControls::cancelAnalysisRequested, [&]() { ++cancels; });
    QObject::connect(&controls, &ccv2::SpikeAnalysisControls::ruleEditorRequested, [&]() { ++launches; });
    check(starts == 0 && analyses == 0, "constructing controls is inert");
    controls.show(); app.processEvents();
    const QString chosenArchive = temporary.path() + "/chosen-archive";
    auto *chooseDestination = child<QPushButton>(controls, "analysisChooseDestination");
    fileDialogAction(chooseDestination, chosenArchive, true);
    check(child<QLineEdit>(controls, "analysisArchiveDestination")->text() == chosenArchive, "archive destination chooser acceptance populates exact path");
    fileDialogAction(chooseDestination, temporary.path() + "/ignored-archive", false);
    check(child<QLineEdit>(controls, "analysisArchiveDestination")->text() == chosenArchive, "archive destination chooser cancellation preserves path");
    auto *offlineToggle = child<QPushButton>(controls, "analysisOfflineToggle"); offlineToggle->click();
    const QString chosenRecording = temporary.path() + "/chosen-recording"; check(QDir().mkpath(chosenRecording), "source folder chooser fixture");
    auto *chooseRecording = child<QPushButton>(controls, "analysisChooseRecording");
    fileDialogAction(chooseRecording, chosenRecording, true);
    check(child<QLineEdit>(controls, "analysisRecordingDirectory")->text() == chosenRecording, "recording chooser acceptance populates exact path");
    fileDialogAction(chooseRecording, temporary.path(), false);
    check(child<QLineEdit>(controls, "analysisRecordingDirectory")->text() == chosenRecording, "recording chooser cancellation preserves path");
    const QString chosenOutput = temporary.path() + "/chosen-offline-output";
    auto *chooseOutput = child<QPushButton>(controls, "analysisChooseOutput");
    fileDialogAction(chooseOutput, chosenOutput, true);
    check(child<QLineEdit>(controls, "analysisOutputDirectory")->text() == chosenOutput, "output chooser acceptance populates exact path");
    fileDialogAction(chooseOutput, temporary.path() + "/ignored-output", false);
    check(child<QLineEdit>(controls, "analysisOutputDirectory")->text() == chosenOutput, "output chooser cancellation preserves path");
    int archiveOpenRequests = 0; QString requestedArchive;
    QObject::connect(&controls, &ccv2::SpikeAnalysisControls::openArchiveRequested, [&](const QString &directory) { ++archiveOpenRequests; requestedArchive = directory; });
    auto *analysisOpen = child<QPushButton>(controls, "analysisOpenArchive");
    fileDialogAction(analysisOpen, path, true);
    check(archiveOpenRequests == 1 && requestedArchive == path, "Open Archive dialog dispatches exact selected archive");
    fileDialogAction(analysisOpen, temporary.path(), false);
    check(archiveOpenRequests == 1, "Open Archive dialog cancel emits no request");
    check(starts == 0 && analyses == 0, "path choosers never implicitly start acquisition or offline work");
    offlineToggle->click();

    controls.setArchiveDestination(QStringLiteral("/tmp/new-sidecar")); controls.setOfflinePaths(QStringLiteral("/tmp/recording"), QStringLiteral("/tmp/offline-output"));
    child<QPushButton>(controls, "analysisStartArchive")->click(); check(starts == 1, "start button request");
    auto status = writer.status(); status.state = ccv2::SpikeArchiveState::Running; status.finishVerified = false; controls.setArchiveStatus(status);
    check(!child<QPushButton>(controls, "analysisStartArchive")->isEnabled(), "running archive start disabled by service status");
    child<QPushButton>(controls, "analysisStopArchive")->click(); check(stops == 1, "stop request does not claim completion");
    status.state = ccv2::SpikeArchiveState::Drained; status.finishVerified = true;
    controls.setArchiveStatus(status, QStringLiteral("/tmp/completed-sidecar"));
    auto *nextDestination = child<QLineEdit>(controls, "analysisArchiveDestination"); nextDestination->setText(QStringLiteral("/tmp/next-sidecar"));
    for (int refresh = 0; refresh < 4; ++refresh) controls.setArchiveStatus(status, QStringLiteral("/tmp/completed-sidecar"));
    check(nextDestination->text() == QStringLiteral("/tmp/next-sidecar"), "terminal status polling preserves editable next destination");

    child<QPushButton>(controls, "analysisOfflineToggle")->click(); child<QPushButton>(controls, "analysisRunOffline")->click(); check(analyses == 1, "offline request");
    controls.setOfflineProgress(40, 100, QStringLiteral("分析中 40 / 100"), true);
    check(child<QProgressBar>(controls, "analysisOfflineProgress")->value() == 400, "offline exact progress reflected");
    child<QPushButton>(controls, "analysisCancelOffline")->click(); check(cancels == 1, "cancel request");
    controls.setOfflineProgress(100, 100, QStringLiteral("正在提交已接受尾部"), false, true);
    check(!child<QPushButton>(controls, "analysisCancelOffline")->isEnabled() && !child<QPushButton>(controls, "analysisRunOffline")->isEnabled(), "finalizing disables both cancel and restart");
    check(!child<QLineEdit>(controls, "analysisRecordingDirectory")->isEnabled() && !child<QPushButton>(controls, "analysisChooseRecording")->isEnabled(), "finalizing locks source field and file chooser");
    controls.setOfflineProgress(40, 100, QStringLiteral("已取消，已排空已接受事件"), false);
    check(!child<QPushButton>(controls, "analysisCancelOffline")->isEnabled(), "terminal cancellation status disables cancel");
    child<QPushButton>(controls, "analysisRuleEditor")->click(); check(launches == 1, "rule launcher request");
    status.state = ccv2::SpikeArchiveState::Failed; status.writerFinished = false; controls.setArchiveStatus(status);
    check(!child<QPushButton>(controls, "analysisStartArchive")->isEnabled(), "failed but draining archive does not invite restart");
    status.writerFinished = true; controls.setArchiveStatus(status);
    check(child<QPushButton>(controls, "analysisStartArchive")->isEnabled(), "failed writer completion permits a new destination request");
    status.state = ccv2::SpikeArchiveState::Drained; controls.setArchiveStatus(status);
    controls.resize(950, 350);

    for (const QString themeName : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("soft-dark"), QStringLiteral("midnight-indigo")}) {
        theme.apply(themeName); const auto p = palette(theme.currentPalette()); browser.setWaveTheme(p); editor.setWaveTheme(p); controls.setWaveTheme(p);
        render(browser, QStringLiteral("archive"), themeName, theme.currentPalette().appBg);
        render(editor, QStringLiteral("rule-editor"), themeName, theme.currentPalette().appBg);
        render(controls, QStringLiteral("analysis-controls"), themeName, theme.currentPalette().appBg);
    }
    theme.apply(QStringLiteral("light")); const auto p = palette(theme.currentPalette()); browser.setWaveTheme(p); editor.setWaveTheme(p); controls.setWaveTheme(p);
    browser.resize(780, 660); editor.resize(780, 680); controls.resize(780, 370);
    render(browser, QStringLiteral("archive"), QStringLiteral("compact"), theme.currentPalette().appBg);
    render(editor, QStringLiteral("rule-editor"), QStringLiteral("compact"), theme.currentPalette().appBg);
    render(controls, QStringLiteral("analysis-controls"), QStringLiteral("compact"), theme.currentPalette().appBg);
    // Clearing requires an explicit future-event application, including the
    // valid empty overall ruleset; it never rewrites retained classifications.
    child<QPushButton>(editor, "ruleClearElectrode")->click(); check(editor.definitions().isEmpty(), "clear electrode yields valid empty definitions");
    apply->click(); check(applyRequests == 2 && editor.definitions().isEmpty(), "explicit empty rules submission");
    browser.close(); editor.close(); controls.close(); app.processEvents();
    check(QThreadPool::globalInstance()->waitForDone(15000), "all cancelled background work terminates");
    std::cout << "spike_archive_rule_widgets_smoke " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
