#include <QtCore>
#include <QtWidgets>
#include <QtNetwork>
#include <atomic>
#include <functional>
#include <memory>
#include <iostream>
#include <vector>
#include <cmath>
#include "core/constants.h"
#include "core/threadsafe_queue.h"
#include "core/frame_timestamp_reconciler.h"
#include "core/channel_routing.h"
#include "network/data_sorter.h"
#include "network/replay_controller.h"
#include "network/timestamp_checker.h"
#include "service/session_hub.h"
#include "io/session_manifest.h"
#include "io/matlab_channel_exporter.h"
#include "signal/spike_filter.h"
#include "signal/spike_snippet_store.h"
#include "data/data_store.h"
#include "network/sorter_worker.h"
#include "core/channel_address_state.h"
#include "core/network_state.h"
#include "config/config_manager.h"
// Test-only access for deterministic GUI fixtures and joined worker inspection.
#define private public
#include "service/spike_detect_worker.h"
#include "ui/waveform_widget.h"
#include "ui/analyzer_panel.h"
#include "ui/spi_control_panel.h"
#undef private

using namespace ccv2;
bool allPassed = true;

bool spinUntil(const std::function<bool()> &ready, int timeout = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    return ready();
}

void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    spinUntil([&] { return timer.elapsed() >= ms; }, ms + 100);
}

QByteArray frame(int timestamp, int adc) {
    QByteArray bytes(kFrameBytes, Qt::Uninitialized);
    const quint32 word = (static_cast<quint32>(timestamp) << kTimestampShift) |
                         (static_cast<quint32>(adc) & kAdcSampleMask);
    for (int ch = 0; ch < kChannelsTotal; ++ch) {
        qToLittleEndian(word, reinterpret_cast<uchar *>(bytes.data() + ch * 4));
    }
    return bytes;
}

QByteArray frames(int first, int count) {
    QByteArray bytes;
    for (int i = first; i < first + count; ++i) bytes += frame(i, 1000 + (i % 4) * 100);
    return bytes;
}

void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("fixture write failed");
}

void report(const char *name, const QJsonObject &values) {
    const QString key = QString::fromLatin1(name);
    bool passed = false;
    if (key == "replay_eof_restart")
        passed = values["completed"].toBool() && values["second_end"].toInt() == 9 && values["epoch_changed"].toBool();
    else if (key == "selected_bin_is_ignored")
        passed = values["completed"].toBool() && values["actual_adc"].toInt() == 999 && values["export_adc"].toInt() == 999;
    else if (key == "truncated_part_export")
        passed = values["export_ok"].toBool() && values["actual_adc"] == values["expected_adc"];
    else if (key == "tdm_resubscribe_after_gap")
        passed = values["completed"].toBool() && values["origin"].toInt() == 65 &&
                 values["continuous_phase_codes"] == QJsonArray{1000,1100,1200,1300} &&
                 values["reattached_phase_codes"] == values["continuous_phase_codes"];
    else if (key == "spike_seek_origin")
        passed = values["actual_next_index"].toInt() == 13 && values["worker_epoch"].toInt() == 1 && values["stale_commit_rejected"].toBool();
    else if (key == "replay_part_disappears")
        passed = values["opened"].toBool() && values["fixture_removed"].toBool() &&
                 !values["still_playing"].toBool() && !values["error"].toString().isEmpty();
    else if (key == "time_waveform_zoom")
        passed = values["red_pixels"].toInt() > 0 && !values["implicit_x_trace_unchanged"].toBool() && values["matches_explicit_x_control"].toBool();
    else if (key == "replay_overwrites_live_rate")
        passed = values["saved_live_rate"].toInt() == 20000;
    else if (key == "constant_input_keeps_old_fft")
        passed = values["initial_fft_valid"].toBool() && !values["old_spectrum_still_displayed"].toBool() && values["current_stats_invalid"].toBool();
    else if (key == "queued_spi_retargets_new_endpoint")
        passed = values["completed"].toBool() && !values["second_command_received_by_A"].toBool() && !values["second_command_received_by_B"].toBool();
    else if (key == "upstream_gap_marked_complete")
        passed = values["completed"].toBool() && !values["manifest_complete"].toBool() && values["manifest_upstream_missing"].toInt() == 1;
    else if (key == "spike_memory_budget")
        passed = values["allocated"].toBool() && values["bytes"].toDouble() <= SpikeSnippetStore::kMemoryBudgetBytes && values["capacity"].toInt() < 2000;
    else if (key == "tdm_export_timeline")
        passed = values["ok"].toBool() && values["phase3_frames"] == QJsonArray{7} && values["loader_has_timeline"].toBool();
    else if (key == "seek_after_gap")
        passed = values["completed"].toBool() && values["first_source_frame"].toInt() == 21 && values["first_adc"].toInt() == 1100;
    else if (key == "queue_epoch_stamp")
        passed = values["delivered_new_epoch"].toBool() && values["dropped_old_epoch"].toBool();
    else if (key == "stale_connection_events")
        passed = values["connected_new_source"].toBool();
    else if (key == "recording_tail_drain")
        passed = values["replay_finished"].toBool() && values["bytes_match"].toBool() && values["manifest_complete"].toBool();
    allPassed = allPassed && passed;
    QJsonObject result = values;
    result.insert("case", key);
    result.insert("passed", passed);
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
}

void replayTwice() {
    QTemporaryDir dir;
    const QString path = dir.filePath("nine_frames.bin");
    save(path, frames(0, 9));
    SessionHub hub;
    int finished = 0;
    QObject::connect(&hub, &SessionHub::replayFinished, [&] { ++finished; });
    hub.startReplay(path, 400.0);
    const quint64 epoch = hub.timelineEpoch();
    hub.replayTogglePlay();
    bool ok = spinUntil([&] { return finished == 1 && hub.currentFrameIndex() == 9; });
    const qint64 firstEnd = hub.currentFrameIndex();
    hub.replayTogglePlay();
    ok = spinUntil([&] { return finished == 2 && hub.currentFrameIndex() == 9; }) && ok;
    report("replay_eof_restart", {{"completed", ok}, {"first_end", firstEnd},
        {"second_end", hub.currentFrameIndex()}, {"expected_second_end", 9},
        {"epoch_changed", epoch != hub.timelineEpoch()}});
    hub.stop();
}

void unrelatedBin() {
    QTemporaryDir dir;
    save(dir.filePath("session_part.bin"), frame(0, 111) + frame(1, 111));
    const QString selected = dir.filePath("unrelated.bin");
    save(selected, frame(0, 999) + frame(1, 999));
    SessionManifestData manifest;
    manifest.complete = true;
    manifest.metadata.sampleRate = 400.0;
    manifest.parts = {{"session_part.bin", 2 * kFrameBytes, 2}};
    SessionManifest::write(dir.path(), manifest);
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    ReplayController replay(queue);
    replay.open(selected, 400.0);
    replay.play();
    const bool ok = spinUntil([&] { return queue->size() > 0; });
    QByteArray chunk;
    queue->pop(chunk, 0);
    const int actual = chunk.isEmpty() ? -1 : int(qFromLittleEndian<quint32>(chunk.constData()) & kAdcSampleMask);
    const auto exported = MatlabChannelExporter::exportAdcBin(selected, dir.filePath("export"), 400.0);
    QFile exportedFile(dir.filePath("export/ch000_i32le.bin"));
    exportedFile.open(QIODevice::ReadOnly);
    const QByteArray exportedBytes = exportedFile.readAll();
    const int exportAdc = exported.ok && exportedBytes.size() >= 4 ? int(qFromLittleEndian<quint32>(exportedBytes.constData())) : -1;
    report("selected_bin_is_ignored", {{"completed", ok}, {"selected_adc", 999}, {"actual_adc", actual}, {"export_adc", exportAdc}});
}

void corruptSplit() {
    QTemporaryDir dir;
    save(dir.filePath("part0.bin"), frame(0, 100) + frame(1, 101) + QByteArray(1, char(0x55)));
    save(dir.filePath("part1.bin"), frame(2, 102) + frame(3, 103));
    SessionManifestData manifest;
    manifest.complete = false;
    manifest.metadata.sampleRate = 400.0;
    manifest.parts = {{"part0.bin", 2 * kFrameBytes + 1, 2}, {"part1.bin", 2 * kFrameBytes, 2}};
    SessionManifest::write(dir.path(), manifest);
    const auto result = MatlabChannelExporter::exportAdcBin(dir.filePath("part0.bin"), dir.filePath("export"), 400.0);
    QFile channel(dir.filePath("export/ch000_i32le.bin"));
    channel.open(QIODevice::ReadOnly);
    const QByteArray data = channel.readAll();
    QJsonArray actual;
    for (int i = 0; i + 4 <= data.size(); i += 4)
        actual.append(int(qFromLittleEndian<quint32>(data.constData() + i)));
    report("truncated_part_export", {{"export_ok", result.ok}, {"actual_adc", actual},
        {"expected_adc", QJsonArray{100, 101, 102, 103}}});
}

void lateTdmSubscriber() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    QPointer<QTcpSocket> peer;
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, peer, [peer] { if (peer) peer->readAll(); });
    });
    SessionHub hub;
    hub.start("127.0.0.1", server.serverPort(), server.serverPort());
    bool ok = spinUntil([&] { return hub.isConnected() && peer; });
    auto q1 = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    auto q2 = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    auto s1 = std::make_shared<RealtimeStreamState>();
    auto s2 = std::make_shared<RealtimeStreamState>();
    s1->channels = s2->channels = {0};
    s1->metricWindowFrames = s2->metricWindowFrames = 4;
    s1->timelineEpoch = s2->timelineEpoch = hub.timelineEpoch();
    std::atomic_bool stop1(false), stop2(false);
    const qint64 firstOrigin = hub.addSubscriberWithFrameOrigin(q1);
    DataSorter early(q1, s1, &stop1, hub.timelineEpochCounter(), nullptr, hub.timelineFrameOriginCounter(), firstOrigin);
    early.start();
    QByteArray prefix;
    for (int i = 0; i < 65; ++i) if (i != 3) prefix += frames(i, 1);
    if (peer) peer->write(prefix);
    ok = spinUntil([&] { QMutexLocker lock(&s1->lock); return s1->totalSamples.value(0) == 65; }) && ok;
    const qint64 origin = hub.addSubscriberWithFrameOrigin(q2);
    DataSorter late(q2, s2, &stop2, hub.timelineEpochCounter(), nullptr, hub.timelineFrameOriginCounter(), origin);
    late.start();
    if (peer) peer->write(frames(65, 64));
    ok = spinUntil([&] {
        QMutexLocker l1(&s1->lock), l2(&s2->lock);
        return s1->totalSamples.value(0) == 129 && s2->totalSamples.value(0) == 129;
    }) && ok;
    hub.removeSubscriber(q1); hub.removeSubscriber(q2);
    stop1.store(true); stop2.store(true); q1->wakeAll(); q2->wakeAll();
    early.wait(); late.wait(); hub.stop();
    QJsonArray a, b;
    for (int p = 0; p < 4; ++p) {
        a.append(qRound(s1->tdmMetrics.value(p).mean * 4096.0 / 1.8));
        b.append(qRound(s2->tdmMetrics.value(p).mean * 4096.0 / 1.8));
    }
    report("tdm_resubscribe_after_gap", {{"completed", ok}, {"origin", origin},
        {"continuous_phase_codes", a}, {"reattached_phase_codes", b}});
}

void spikeSeek() {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    SpikeSnippetStore store;
    store.configure(512, 3, 20);
    SpikeDetectConfig cfg;
    cfg.tdmEnabled = true;
    cfg.preSamples = cfg.postSamples = 1;
    cfg.proc.sampleRate = 5000.0;
    std::atomic_bool stop(false);
    std::atomic<quint64> epoch(0);
    std::atomic<qint64> origin(0);
    SpikeDetectWorker worker(queue, &store, &stop, cfg, &epoch, nullptr, 0, nullptr, &origin);
    const auto pushBlock = [&](quint64 blockEpoch, int first, int count) {
        StreamBlockInfo info;
        info.epoch = blockEpoch;
        for (int i = 0; i < count; ++i) info.frameIndices.push_back(first + i);
        queue->push(frames(first, count), false, nullptr, info);
    };
    worker.start();
    pushBlock(0, 0, 8);
    pump(150);
    // Main-window seek clears the retained store and advances the epoch.
    store.resetTimeline(1);
    origin.store(5);
    epoch.store(1);
    pushBlock(1, 5, 4);
    pump(150);
    pushBlock(0, 400, 4);  // delayed old chunk must never commit after the seek
    pushBlock(1, 9, 4);
    pump(150);
    stop.store(true); queue->wakeAll(); worker.wait();
    report("spike_seek_origin", {{"actual_next_index", worker.m_nextFrameIndex},
        {"expected_next_index", 13}, {"worker_epoch", qint64(worker.m_lastEpoch)},
        {"stale_commit_rejected", !store.addSnippetIfEpoch(0, QVector<float>(3, 0.0f).constData(), 0)}});
}

void missingReplayPart() {
    QTemporaryDir dir;
    save(dir.filePath("part0.bin"), frames(0, 4));
    save(dir.filePath("part1.bin"), frames(4, 4));
    SessionManifestData manifest;
    manifest.complete = true;
    manifest.metadata.sampleRate = 400.0;
    manifest.parts = {{"part0.bin", 4 * kFrameBytes, 4}, {"part1.bin", 4 * kFrameBytes, 4}};
    SessionManifest::write(dir.path(), manifest);
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    ReplayController replay(queue);
    const bool opened = replay.open(dir.filePath("part0.bin"), 400.0);
    const bool removed = QFile::remove(dir.filePath("part1.bin"));
    replay.play();
    pump(300);
    report("replay_part_disappears", {{"opened", opened}, {"fixture_removed", removed},
        {"still_playing", replay.isPlaying()}, {"current_frame", replay.currentFrame()},
        {"total_frames", replay.totalFrames()}, {"error", replay.errorString()}});
}

QByteArray redMask(const QImage &image, const QRect &plot) {
    QByteArray mask;
    for (int y = plot.top() + 2; y < plot.bottom() - 2; ++y) {
        for (int x = plot.left() + 2; x < plot.right() - 2; ++x) {
            const QColor color = image.pixelColor(x, y);
            mask.append(color.red() > 180 && color.green() < 90 && color.blue() < 90 ? '1' : '0');
        }
    }
    return mask;
}

void waveformZoom() {
    WaveformWidget widget("Time-domain ramp");
    widget.resize(640, 400);
    widget.setXRange(0.0, 1.0);
    widget.setYRange(0.0, 1.0);
    WaveformWidget::PlotSeries series;
    series.color = Qt::red;
    for (int i = 0; i <= 1000; ++i) series.data.append(i / 1000.0);
    widget.setSeries({series});
    const auto render = [&] {
        QImage image(widget.size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget.render(&image);
        return image;
    };
    const QImage before = render();
    widget.m_zoomActive = true;
    widget.m_zoomXMin = 0.25;
    widget.m_zoomXMax = 0.75;
    widget.m_zoomYMin = 0.0;
    widget.m_zoomYMax = 1.0;
    const QImage after = render();
    const QRect plot = widget.plotRectForRange(0.0, 1.0);
    const QByteArray maskBefore = redMask(before, plot);
    const QByteArray maskAfter = redMask(after, plot);
    for (int i = 0; i <= 1000; ++i) series.xData.append(i / 1000.0);
    widget.setSeries({series});
    const QImage explicitX = render();
    const QString artifacts = qEnvironmentVariable("CCV2_REVIEW_ARTIFACTS");
    if (!artifacts.isEmpty()) {
        QDir().mkpath(artifacts);
        before.save(QDir(artifacts).filePath("zoom_before.png"));
        after.save(QDir(artifacts).filePath("zoom_fixed.png"));
        explicitX.save(QDir(artifacts).filePath("zoom_explicit_control.png"));
    }
    report("time_waveform_zoom", {{"red_pixels", maskBefore.count('1')},
        {"implicit_x_trace_unchanged", maskBefore == maskAfter},
        {"matches_explicit_x_control", maskAfter == redMask(explicitX, plot)},
        {"new_axis_start", widget.displayedXRange().first},
        {"new_axis_end", widget.displayedXRange().second}});
}

void replayRatePersistence() {
    QTemporaryDir dir;
    ConfigManager config(dir.filePath("review_config.ini"));
    if (QFileInfo(config.path()).absoluteFilePath() != dir.filePath("review_config.ini"))
        qFatal("review config must stay in the temporary fixture directory");
    ConfigMap values = config.load();
    values["Signal"]["sampling_rate"] = "20000";
    config.save(values);
    save(dir.filePath("part0.bin"), frames(0, 8));
    SessionManifestData manifest;
    manifest.complete = true;
    manifest.metadata.sampleRate = 40000.0;
    manifest.parts = {{"part0.bin", 8 * kFrameBytes, 8}};
    SessionManifest::write(dir.path(), manifest);
    SessionHub hub;
    NetworkState network("127.0.0.1", 1, 1);
    ChannelAddressState channels;
    AnalyzerPanel panel(&config, &network, &channels);
    panel.setSessionHub(&hub);
    hub.startReplay(dir.filePath("part0.bin"), 20000.0);
    panel.onActivated();
    pump(650);
    const int saved = config.load().value("Signal").value("sampling_rate").toInt();
    report("replay_overwrites_live_rate", {{"original_live_rate", 20000},
        {"recording_rate", 40000}, {"saved_live_rate", saved}});
    panel.shutdown();
    hub.stop();
}

void staleFft() {
    QTemporaryDir dir;
    ConfigManager config(dir.filePath("review_config.ini"));
    ConfigMap values = config.load();
    values["Analyzer"]["channels"] = "0";
    values["Signal"]["sampling_rate"] = "20000";
    values["Signal"]["fft_points"] = "2048";
    values["Signal"]["fft_enabled"] = "1";
    config.save(values);
    NetworkState network("127.0.0.1", 1, 1);
    ChannelAddressState channels;
    AnalyzerPanel panel(&config, &network, &channels);
    panel.m_isRunning = true;
    panel.m_channels = {0};
    panel.m_groups = {{0}};
    panel.m_dataStore.reset({0}, 8192, 0, 0);
    QVector<qint32> sine;
    for (int i = 0; i < 2048; ++i)
        sine.append(qRound(2048.0 + 500.0 * std::sin(2.0 * 3.14159265358979323846 * 50.0 * i / 2048.0)));
    panel.m_dataStore.appendChannelValues(0, sine);
    panel.updatePlots();
    const bool valid = spinUntil([&] { return !panel.m_fftBusy.load() && !panel.m_fftPlot->m_series.isEmpty(); });
    const QVector<double> previous = valid ? panel.m_fftPlot->m_series[0].data : QVector<double>{};
    panel.m_dataStore.reset({0}, 8192, 0, 0);
    panel.m_dataStore.appendChannelValues(0, QVector<qint32>(2048, 2048));
    panel.updatePlots();
    spinUntil([&] { return !panel.m_fftBusy.load(); });
    const bool unchanged = valid && !panel.m_fftPlot->m_series.isEmpty() && panel.m_fftPlot->m_series[0].data == previous;
    bool invalidCurrent = false;
    for (auto *table : panel.findChildren<QTableWidget *>()) {
        if (table->rowCount() > 0 && table->item(0, 0) && table->item(0, 0)->text() == "Fin")
            invalidCurrent = table->item(0, 1)->text() == "-";
    }
    report("constant_input_keeps_old_fft", {{"initial_fft_valid", valid}, {"old_spectrum_still_displayed", unchanged},
        {"current_stats_invalid", invalidCurrent}});
    panel.shutdown();
}

void queuedSpiChangesDevice() {
    QTemporaryDir dir;
    ConfigManager config(dir.filePath("review_config.ini"));
    QTcpServer first, second;
    if (!first.listen(QHostAddress::LocalHost, 0) || !second.listen(QHostAddress::LocalHost, 0))
        qFatal("cannot listen on loopback fixture ports");
    QByteArray firstBytes, secondBytes;
    const auto collect = [](QTcpServer &server, QByteArray &bytes) {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &bytes] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &bytes] { bytes += socket->readAll(); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    };
    collect(first, firstBytes);
    collect(second, secondBytes);
    NetworkState network("127.0.0.1", first.serverPort(), first.serverPort());
    ChannelAddressState channels;
    SpiControlPanel panel(&config, &network, &channels);
    const QString filler(32, QLatin1Char('0'));
    const QString secondCommand = "00011000000000000000000000000001";
    bool drained = false;
    QObject::connect(&panel, &SpiControlPanel::busyChanged, [&](bool busy) { if (!busy) drained = true; });
    panel.enqueueExternalCommands("first queued for A", {filler, filler, filler, filler}, 30000);
    panel.enqueueExternalCommands("second queued for A", {secondCommand}, 0);
    network.setEndpoint("127.0.0.1", second.serverPort(), second.serverPort());
    const bool done = spinUntil([&] { return drained; }, 3000);
    pump(50);
    report("queued_spi_retargets_new_endpoint", {{"completed", done},
        {"second_command_received_by_A", firstBytes.contains("spi18000001")},
        {"second_command_received_by_B", secondBytes.contains("spi18000001")}});
}

void upstreamGapRecordingCompleteness() {
    QTemporaryDir dir;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) qFatal("loopback fixture listen failed");
    QByteArray source;
    for (int i = 0; i < 65; ++i) if (i != 3) source += frames(i, 1);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, source] {
                if (socket->readAll().startsWith("ctre"))
                    QTimer::singleShot(50, socket, [socket, source] { socket->write(source); });
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    SessionHub hub;
    bool recordingStarted = false;
    QObject::connect(&hub, &SessionHub::connectionStateChanged, [&](bool connected) {
        if (connected) recordingStarted = hub.startRecording(dir.path(), "recording");
    });
    hub.start("127.0.0.1", server.serverPort(), server.serverPort(), "ctre", 20000.0);
    const bool gotData = spinUntil([&] { return hub.recordedBytes() == source.size(); });
    hub.stopRecording();
    const QString folder = hub.recordingPath();
    hub.stop();
    SessionManifestData manifest;
    const bool loaded = SessionManifest::read(folder, &manifest);
    TimestampContinuityAnalyzer analyzer(1);
    analyzer.process(source);
    report("upstream_gap_marked_complete", {{"completed", gotData && recordingStarted && loaded},
        {"actual_missing_frames", analyzer.stats().estimatedMissingFrames},
        {"manifest_ingress_dropped", manifest.ingressDroppedFrames}, {"manifest_complete", manifest.complete},
        {"manifest_upstream_missing", manifest.integrity.upstreamMissingFrames}});
}


void memoryBudget() {
    SpikeSnippetStore store;
    const bool ok = store.configure(256, 6001, 2000);
    report("spike_memory_budget", {{"allocated", ok}, {"capacity", store.capacity()},
        {"bytes", qint64(store.channels()) * store.capacity() * store.snippetLength() * 4}});
}

void tdmExportTimeline() {
    QTemporaryDir dir;
    QByteArray bytes;
    for (int f : {0, 1, 2, 4, 5, 6, 7, 8, 9}) bytes += frame(f, f);
    save(dir.filePath("input.bin"), bytes);
    MatlabChannelExportOptions options;
    options.mode = MatlabExportMode::TdmDemux;
    const auto result = MatlabChannelExporter::exportAdcBin(dir.filePath("input.bin"), dir.filePath("export"), 20000, options);
    QFile idx(dir.filePath("export/tdm_phase3_frames_i64le.bin"));
    idx.open(QIODevice::ReadOnly);
    const auto data = idx.readAll();
    QJsonArray positions;
    for (int i = 0; i + 8 <= data.size(); i += 8) positions.append(qFromLittleEndian<qint64>(data.constData() + i));
    QFile loader(dir.filePath("export/read_tdm_timeline.m"));
    loader.open(QIODevice::ReadOnly);
    const QByteArray script = loader.readAll();
    report("tdm_export_timeline", {{"ok", result.ok}, {"phase3_frames", positions},
        {"loader_has_timeline", script.contains("int64=>int64") && script.contains("20000")}});
}

void seekAfterGap() {
    QTemporaryDir dir;
    QByteArray bytes;
    for (int f = 0; f < 81; ++f) if (f != 3) bytes += frames(f, 1);
    save(dir.filePath("input.bin"), bytes);
    SessionHub hub;
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    hub.addSubscriber(queue);
    bool ok = hub.startReplay(dir.filePath("input.bin"), 400.0);
    hub.replaySeekFraction(0.25); // file frame 20 is source frame 21
    hub.replayTogglePlay();
    ok = spinUntil([&] { return queue->size() > 0; }) && ok;
    QByteArray chunk;
    StreamBlockInfo info;
    queue->pop(chunk, 0, nullptr, &info);
    const int code = chunk.size() >= 4 ? int(qFromLittleEndian<quint32>(chunk.constData()) & kAdcSampleMask) : -1;
    report("seek_after_gap", {{"completed", ok && info.valid()}, {"first_source_frame", info.valid() ? info.firstFrame() : -1},
        {"first_adc", code}});
    hub.stop();
}

void queueEpochStamp() {
    auto queue = std::make_shared<ThreadSafeQueue<QByteArray>>(16);
    auto state = std::make_shared<RealtimeStreamState>();
    std::atomic_bool stop(false);
    std::atomic<quint64> epoch(2);
    state->timelineEpoch = 2;
    state->channels = {0};
    state->metricWindowFrames = 1;
    DataSorter sorter(queue, state, &stop, &epoch);
    sorter.start();
    StreamBlockInfo old;
    old.epoch = 1;
    old.frameIndices = {0, 1, 2, 3};
    queue->push(frames(0, 4), false, nullptr, old);
    pump(60);
    bool discarded;
    { QMutexLocker lock(&state->lock); discarded = state->buffers.value(0).isEmpty(); }
    StreamBlockInfo fresh;
    fresh.epoch = 2;
    fresh.frameIndices = {100, 101, 102, 103};
    queue->push(frames(100, 4), false, nullptr, fresh);
    const bool delivered = spinUntil([&] { QMutexLocker lock(&state->lock); return state->totalSamples.value(0) == 104; });
    stop.store(true); queue->wakeAll(); sorter.wait();
    report("queue_epoch_stamp", {{"delivered_new_epoch", delivered}, {"dropped_old_epoch", discarded}});
}

void staleConnectionEvents() {
    QTcpServer reserved;
    reserved.listen(QHostAddress::LocalHost, 0);
    const quint16 badPort = reserved.serverPort();
    reserved.close();
    SessionHub hub;
    hub.start("127.0.0.1", badPort, badPort);
    QThread::msleep(100);  // leave old connection-error callbacks queued on GUI
    hub.stop();
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); });
        }
    });
    hub.start("127.0.0.1", server.serverPort(), server.serverPort());
    const bool connected = spinUntil([&] { return hub.state() == SessionHub::State::Live; });
    report("stale_connection_events", {{"connected_new_source", connected}});
    hub.stop();
}

void recordingTailDrain() {
    QTemporaryDir dir;
    const QByteArray source = frames(0, 10000);
    save(dir.filePath("input.bin"), source);
    SessionHub hub;
    bool finished = false;
    QObject::connect(&hub, &SessionHub::replayFinished, [&] { finished = true; });
    hub.startReplay(dir.filePath("input.bin"), 1000000.0);
    hub.startRecording(dir.path(), "capture");
    hub.replayTogglePlay();
    spinUntil([&] { return finished; });
    hub.stop(); // no wait for the distributor: stop itself must drain its tail
    QFile recorded(QDir(hub.recordingPath()).filePath("ADC_DATA.bin"));
    recorded.open(QIODevice::ReadOnly);
    SessionManifestData manifest;
    SessionManifest::read(hub.recordingPath(), &manifest);
    report("recording_tail_drain", {{"replay_finished", finished},
        {"bytes_match", recorded.readAll() == source}, {"manifest_complete", manifest.complete}});
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("NLReviewTests");
    QCoreApplication::setApplicationName("IsolatedRegression");
    const struct { const char *name; void (*run)(); } cases[] = {
        {"eof", replayTwice}, {"input", unrelatedBin}, {"split", corruptSplit},
        {"tdm", lateTdmSubscriber}, {"spike_seek", spikeSeek}, {"io_error", missingReplayPart},
        {"zoom", waveformZoom}, {"sample_rate", replayRatePersistence}, {"fft", staleFft},
        {"spi_target", queuedSpiChangesDevice}, {"record_integrity", upstreamGapRecordingCompleteness},
        {"memory", memoryBudget}, {"export_time", tdmExportTimeline}, {"seek_gap", seekAfterGap},
        {"epoch", queueEpochStamp}, {"connection_epoch", staleConnectionEvents}, {"record_tail", recordingTailDrain}
    };
    bool found = false;
    for (const auto &test : cases) {
        if (argc == 1 || QString::fromLocal8Bit(argv[1]) == QLatin1String(test.name)) {
            found = true;
            test.run();
        }
    }
    return found && allPassed ? 0 : 1;
}
