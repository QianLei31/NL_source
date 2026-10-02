#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <iostream>

#include "config/config_manager.h"
#include "core/constants.h"
#include "core/tdm_context.h"
#include "service/session_hub.h"
#include "ui/spike_panel.h"

namespace {
bool waitFor(const std::function<bool()> &predicate, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) return true;
        QThread::msleep(2);
    }
    return predicate();
}

void pump(int duration) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < duration) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

QByteArray makeFrames(int count) {
    QByteArray data(count * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int frame = 0; frame < count; ++frame) {
        for (int channel = 0; channel < ccv2::kChannelsTotal; ++channel) {
            // Large, deterministic negative transients, apart from startup.
            const int sample = channel == 0 && frame % 200 >= 80 && frame % 200 < 84
                                   ? 1700 : 2048;
            const quint32 raw = (static_cast<quint32>(frame) << ccv2::kTimestampShift) |
                                static_cast<quint32>(sample);
            qToLittleEndian<quint32>(raw, reinterpret_cast<uchar *>(data.data() +
                frame * ccv2::kFrameBytes + channel * ccv2::kBytesPerPoint));
        }
    }
    return data;
}

struct Snapshot {
    QVector<qint64> exposure;
    QVector<qint64> totals;
    ccv2::SpikeAnalysisQuality quality;
};

Snapshot snapshot(const ccv2::SpikePanel &panel) {
    Snapshot result;
    panel.analysisStore()->snapshotAnalysis(&result.exposure, &result.quality, &result.totals);
    return result;
}

bool check(bool condition, const char *message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QString binPath = tmp.filePath(QStringLiteral("source.bin"));
    constexpr int kFrames = 18000;
    constexpr double kSampleRate = 10000.0;
    const QByteArray frames = makeFrames(kFrames);
    QFile bin(binPath);
    if (!bin.open(QIODevice::WriteOnly) || bin.write(frames) != frames.size()) return 2;
    bin.close();

    ccv2::ConfigManager config(tmp.filePath(QStringLiteral("config.ini")));
    ccv2::ConfigMap settings;
    settings[QStringLiteral("SpikePanel")] = {
        {QStringLiteral("threshold_units"), QStringLiteral("input_referred")},
        {QStringLiteral("input_gain"), QStringLiteral("60")},
        {QStringLiteral("threshold_mode"), QStringLiteral("1")},
        {QStringLiteral("threshold_value"), QStringLiteral("50")},
        {QStringLiteral("retain"), QStringLiteral("20")}
    };
    if (!config.save(settings)) return 3;

    ccv2::SessionHub hub;
    ccv2::SpikePanel panel(&config);
    panel.resize(1500, 900);
    panel.setSessionHub(&hub);
    panel.setSessionHub(&hub); // repeated bind must not duplicate callbacks
    if (!check(hub.startReplay(binPath, kSampleRate), "open replay")) return 4;
    if (!check(panel.analysisRunning(), "analysis must start before spike tab is opened")) return 5;
    if (!check(hub.analysisMetadata().value("source_sample_rate_hz").toDouble() == kSampleRate,
               "recording analysis provenance was not initialized")) return 31;
    hub.replayTogglePlay();
    if (!check(waitFor([&] { return snapshot(panel).exposure.value(0) >= 3000; }),
               "hidden detector did not analyze source")) return 6;
    const Snapshot hidden = snapshot(panel);
    if (!check(hidden.totals.value(0) > 0, "hidden detector has no events")) return 7;

    panel.show();
    panel.onActivated();
    const auto *replayNote = panel.findChild<QLabel *>(QStringLiteral("spikeAnalysisQuality"));
    if (!check(replayNote && replayNote->text().contains(QStringLiteral("未记录神经分析参数")) &&
               replayNote->text().contains(QStringLiteral("来源完整性未证实")),
               "raw BIN source-only provenance falsely claims recorded analysis settings")) return 35;
    panel.onDeactivated();
    if (!check(panel.analysisRunning(), "tab deactivation stopped analysis")) return 8;
    if (!check(waitFor([&] { return snapshot(panel).exposure.value(0) >= 6000; }),
               "source exposure stopped while hidden")) return 9;
    const Snapshot switched = snapshot(panel);
    if (!check(switched.quality.epoch == hidden.quality.epoch &&
               switched.totals.value(0) > hidden.totals.value(0) &&
               switched.quality.firstSourceFrame == hidden.quality.firstSourceFrame,
               "tab switching reset the scientific interval")) return 10;

    panel.onActivated();
    auto *pause = panel.findChild<QPushButton *>(QStringLiteral("spikePauseDisplay"));
    if (!check(pause != nullptr, "missing display pause control")) return 11;
    pause->setChecked(true);
    const qint64 pauseStart = snapshot(panel).exposure.value(0);
    if (!check(waitFor([&] { return snapshot(panel).exposure.value(0) >= pauseStart + 1500; }),
               "display pause stopped source analysis")) return 12;
    if (!check(panel.analysisRunning(), "display pause stopped worker")) return 13;

    hub.replayPause();
    // Let queued chunks finish, then verify that wall time cannot dilute rates.
    pump(200);
    QMetaObject::invokeMethod(&panel, "refresh", Qt::DirectConnection);
    const Snapshot paused = snapshot(panel);
    const QVector<double> rateBefore = panel.analysisRatesHz();
    const double expectedRate = paused.totals.value(0) * kSampleRate /
                                qMax<qint64>(1, paused.exposure.value(0));
    if (!check(std::abs(rateBefore.value(0) - expectedRate) < 1e-9,
               "rate denominator is not analyzed source time")) return 14;
    pump(650);
    QMetaObject::invokeMethod(&panel, "refresh", Qt::DirectConnection);
    if (!check(snapshot(panel).exposure == paused.exposure &&
               panel.analysisRatesHz() == rateBefore,
               "replay pause changed source-time rate or exposure")) return 15;
    pause->setChecked(false);
    const QString screenshotPath = qEnvironmentVariable("NL_SPIKE_PANEL_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        QApplication::processEvents();
        if (!check(panel.grab().save(screenshotPath), "save populated panel screenshot")) return 36;
    }

    const quint64 epochBeforeSeek = hub.timelineEpoch();
    hub.replaySeekFraction(0.6);
    const Snapshot sought = snapshot(panel);
    if (!check(hub.timelineEpoch() != epochBeforeSeek &&
               sought.quality.epoch == hub.timelineEpoch() &&
               sought.exposure.value(0) == 0 && sought.totals.value(0) == 0,
               "seek did not reset events and exposure together")) return 16;
    hub.replayTogglePlay();
    if (!check(waitFor([&] { return snapshot(panel).exposure.value(0) > 1000; }),
               "analysis failed to resume after seek")) return 17;
    hub.replayPause();
    pump(150);
    const Snapshot afterSeek = snapshot(panel);
    if (!check(afterSeek.quality.firstSourceFrame >= 10000,
               "post-seek source identity is stale")) return 18;

    // A configuration change while another page is visible starts a new
    // coherent interval without depending on an activation callback.
    panel.onDeactivated();
    hub.tdmContext()->setEnabled(true);
    if (!check(panel.analysisRunning() && panel.analysisStore()->channels() == 512 &&
               snapshot(panel).exposure.value(0) == 0,
               "hidden TDM configuration change did not rebuild detector")) return 19;
    hub.replayTogglePlay();
    if (!check(waitFor([&] { return snapshot(panel).exposure.value(0) > 100; }),
               "TDM detector failed after configuration restart")) return 20;
    hub.replayPause();
    pump(150);
    panel.onActivated();
    QMetaObject::invokeMethod(&panel, "refresh", Qt::DirectConnection);
    const Snapshot tdm = snapshot(panel);
    if (!check(std::abs(panel.analysisRatesHz().value(0) -
                        tdm.totals.value(0) * (kSampleRate / 4) /
                        qMax<qint64>(1, tdm.exposure.value(0))) < 1e-9,
               "TDM source-time rate uses wrong stride")) return 21;

    // Normal stop joins the producer/distributor and drains accepted detector
    // input. The final snapshot can advance; raw-source validity stays unknown.
    const Snapshot beforeStop = snapshot(panel);
    hub.stop();
    const Snapshot stopped = snapshot(panel);
    bool monotonic = stopped.exposure.size() == beforeStop.exposure.size() &&
                     stopped.totals.size() == beforeStop.totals.size();
    for (int lane = 0; monotonic && lane < stopped.exposure.size(); ++lane) {
        monotonic = stopped.exposure[lane] >= beforeStop.exposure[lane] &&
                    stopped.totals[lane] >= beforeStop.totals[lane];
    }
    if (!check(!panel.analysisRunning() && monotonic &&
               stopped.quality.pendingWindowEvents == 0 && !stopped.quality.stoppedEarly &&
               stopped.quality.unverifiedFrames > 0 && stopped.quality.incomplete(),
               "normal stop failed to drain accepted data while retaining raw-source uncertainty")) return 22;
    panel.onDeactivated();
    panel.onActivated();
    if (!check(snapshot(panel).totals == stopped.totals && !panel.analysisRunning(),
               "reactivating stopped session restarted or cleared analysis")) return 23;
    const auto *qualityLabel = panel.findChild<QLabel *>(QStringLiteral("spikeAnalysisQuality"));
    if (!check(qualityLabel && qualityLabel->text().contains(QStringLiteral("覆盖不完整")),
               "partial coverage is not visible")) return 24;
    if (!check(qualityLabel->text().contains(QStringLiteral("未验证有效性")),
               "legacy raw-source validity is not disclosed")) return 32;

    hub.tdmContext()->setEnabled(false);
    if (!check(!panel.analysisRunning() && panel.analysisStore()->channels() == 256 &&
               snapshot(panel).totals.value(0) == 0 && snapshot(panel).exposure.value(0) == 0,
               "stopped mapping change retained old-mode events")) return 33;

    // A new replay resets old partial flags; clear is an explicit interval reset.
    if (!hub.startReplay(binPath, kSampleRate)) return 25;
    if (!check(panel.analysisRunning() && !snapshot(panel).quality.incomplete(),
               "new session inherited partial-quality status")) return 26;
    hub.replayTogglePlay();
    if (!waitFor([&] { return snapshot(panel).exposure.value(0) > 100; })) return 27;
    hub.replayPause();
    pump(150);
    QMetaObject::invokeMethod(&panel, "clearAll", Qt::DirectConnection);
    if (!check(panel.analysisRunning() && snapshot(panel).exposure.value(0) == 0 &&
               snapshot(panel).totals.value(0) == 0,
               "explicit clear did not reset coherent analysis interval")) return 28;

    const Snapshot beforeUnbind = snapshot(panel);
    panel.setSessionHub(nullptr);
    if (!check(!panel.analysisRunning() && snapshot(panel).totals == beforeUnbind.totals &&
               snapshot(panel).quality.stoppedEarly,
               "hub unbind did not preserve and qualify retained snapshot")) return 37;
    panel.setSessionHub(&hub);
    if (!check(panel.analysisRunning() && !snapshot(panel).quality.stoppedEarly,
               "hub rebind did not start a fresh analysis interval")) return 38;

    panel.shutdown();
    if (!check(snapshot(panel).quality.stoppedEarly,
               "shutdown left analysis snapshot marked complete")) return 34;
    hub.stop();
    if (!hub.startReplay(binPath, kSampleRate)) return 29;
    if (!check(!panel.analysisRunning(), "shutdown panel reattached to new session")) return 30;
    hub.stop();
    std::cout << "continuous_neural_analysis_smoke ok\n";
    return 0;
}
