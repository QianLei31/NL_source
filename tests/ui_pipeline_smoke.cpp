#include "core/constants.h"
#include "core/channel_routing.h"
#include "core/threadsafe_queue.h"
#include "data/data_store.h"
#include "io/matlab_channel_exporter.h"
#include "network/data_sorter.h"
#include "network/sorter_worker.h"
#include "io/session_recorder.h"
#include "io/session_manifest.h"
#include "signal/sndr_calculator.h"
#include "ui/waveform_widget.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QQueue>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFrames = 2048;
constexpr int kToneCycles = 32;

QByteArray makeFrames()
{
    QByteArray frames(kFrames * ccv2::kFrameBytes, Qt::Uninitialized);
    for (int f = 0; f < kFrames; ++f) {
        const double phase = 2.0 * kPi * static_cast<double>(kToneCycles) *
                             static_cast<double>(f) / static_cast<double>(kFrames);
        const qint32 base = static_cast<qint32>(2048.0 + 1800.0 * std::sin(phase));
        for (int ch = 0; ch < ccv2::kChannelsTotal; ++ch) {
            const qint32 value = qBound<qint32>(0, base + (ch % 17), 4095);
            char *dst = frames.data() + f * ccv2::kFrameBytes + ch * ccv2::kBytesPerPoint;
            const quint32 raw =
                ((static_cast<quint32>(f) & 0xFFFFFu) << ccv2::kTimestampShift) |
                static_cast<quint32>(value);
            qToLittleEndian<quint32>(raw, reinterpret_cast<uchar *>(dst));
        }
    }
    return frames;
}

bool waitFor(const std::function<bool()> &pred, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (pred()) {
            return true;
        }
        QThread::msleep(20);
    }
    return pred();
}

bool imageHasInk(const QImage &img)
{
    const QRgb first = img.pixel(0, 0);
    int different = 0;
    for (int y = 0; y < img.height(); y += 4) {
        for (int x = 0; x < img.width(); x += 4) {
            if (img.pixel(x, y) != first) {
                ++different;
                if (different > 20) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool renderWidget(ccv2::WaveformWidget &widget, const QString &path)
{
    widget.resize(640, 240);
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    return imageHasInk(image) && image.save(path);
}

}  // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication app(argc, argv);

    const QByteArray frames = makeFrames();
    const QVector<int> channels{0, 5, 199};

    {
        ccv2::ThreadSafeQueue<QByteArray> queue(1);
        const QByteArray first = frames.left(ccv2::kFrameBytes);
        const QByteArray second = frames.mid(ccv2::kFrameBytes, ccv2::kFrameBytes);
        QByteArray dropped;
        if (!queue.push(first, false) || queue.push(second, true, &dropped) ||
            dropped != first) {
            std::cerr << "ThreadSafeQueue drop reporting failed" << std::endl;
            return 1;
        }
        QByteArray remaining;
        if (!queue.pop(remaining, 0) || remaining != second) {
            std::cerr << "ThreadSafeQueue retained wrong item" << std::endl;
            return 2;
        }
    }

    {
        ccv2::RingBuffer buffer(4, 10);
        buffer.appendMany(QVector<qint32>{1, 2, 3, 4, 5, 6});
        const auto latest = buffer.getLatest(4);
        if (latest.first != QVector<qint64>({12, 13, 14, 15}) ||
            latest.second != QVector<qint32>({3, 4, 5, 6})) {
            std::cerr << "RingBuffer global index after clipping is invalid" << std::endl;
            return 3;
        }
    }

    {
        auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(16);
        auto state = std::make_shared<ccv2::RealtimeStreamState>();
        std::atomic_bool stopFlag{false};
        {
            QMutexLocker locker(&state->lock);
            state->channels = channels;
            for (int ch : channels) {
                state->buffers[ch] = QQueue<double>();
            }
        }

        ccv2::DataSorter sorter(queue, state, &stopFlag);
        sorter.start();
        queue->push(frames.left(777), true);
        queue->push(frames.mid(777, 19117), true);
        queue->push(frames.mid(777 + 19117), true);

        const bool ok = waitFor([&]() {
            QMutexLocker locker(&state->lock);
            for (int ch : channels) {
                if (state->buffers.value(ch).size() < 1200) {
                    return false;
                }
            }
            return true;
        }, 3000);
        // A hidden display page must discard backlog and stop promptly; tab
        // switching runs this path on the GUI thread.
        for (int i = 0; i < 12; ++i) {
            queue->push(frames, true);
        }
        QElapsedTimer stopTimer;
        stopTimer.start();
        stopFlag.store(true);
        queue->wakeAll();
        const bool stoppedPromptly = sorter.wait(500);
        if (!stoppedPromptly || stopTimer.elapsed() >= 500) {
            std::cerr << "DataSorter stop latency would block page switching" << std::endl;
            return 3;
        }
        if (!ok) {
            std::cerr << "DataSorter did not fill realtime buffers" << std::endl;
            return 3;
        }
    }

    {
        auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(4);
        auto state = std::make_shared<ccv2::RealtimeStreamState>();
        std::atomic_bool stopFlag{false};
        std::atomic<quint64> epoch{7};
        std::atomic<qint64> origin{1};
        {
            QMutexLocker locker(&state->lock);
            state->channels = {0};
            state->timelineEpoch = 7;
            state->metricWindowFrames = 2;
        }
        ccv2::DataSorter sorter(queue,
                                state,
                                &stopFlag,
                                &epoch,
                                nullptr,
                                &origin,
                                1);
        sorter.start();
        queue->push(frames.left(2 * ccv2::kFrameBytes), false);
        const bool ok = waitFor([&]() {
            QMutexLocker locker(&state->lock);
            return state->tdmMetrics.size() == ccv2::kChannelsTotal * ccv2::kTdmPhaseCount &&
                   state->tdmMetrics[1].valid && state->tdmMetrics[2].valid;
        }, 2000);
        stopFlag.store(true);
        queue->wakeAll();
        sorter.wait(1000);
        if (!ok) {
            std::cerr << "TDM metrics did not publish" << std::endl;
            return 4;
        }
        const quint32 raw0 = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(frames.constData()));
        const quint32 raw1 = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(frames.constData() + ccv2::kFrameBytes));
        const double v0 = static_cast<double>(raw0 & ccv2::kAdcSampleMask) / 4096.0 * 1.8;
        const double v1 = static_cast<double>(raw1 & ccv2::kAdcSampleMask) / 4096.0 * 1.8;
        QMutexLocker locker(&state->lock);
        if (std::abs(state->tdmMetrics[1].mean - v0) > 1e-12 ||
            std::abs(state->tdmMetrics[2].mean - v1) > 1e-12) {
            std::cerr << "TDM global frame origin phase is reversed" << std::endl;
            return 5;
        }
    }

    ccv2::DataStore dataStore;
    dataStore.reset(channels, kFrames);
    {
        auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(16);
        ccv2::SessionRecorder *recorder = nullptr;
        ccv2::SessionRecorder dummyRecorder;
        recorder = &dummyRecorder;
        std::atomic_bool stopFlag{false};
        ccv2::SorterWorker worker(queue,
                                  &dataStore,
                                  channels,
                                  recorder,
                                  []() { return false; },
                                  []() { return true; },
                                  &stopFlag);
        worker.start();
        queue->push(frames.left(513), false);
        queue->push(frames.mid(513, 32768), false);
        queue->push(frames.mid(513 + 32768), false);
        const bool ok = waitFor([&]() {
            for (int ch : channels) {
                auto *buf = dataStore.buffer(ch);
                if (!buf || buf->getLatest(kFrames).second.size() != kFrames) {
                    return false;
                }
            }
            return true;
        }, 3000);
        for (int i = 0; i < 12; ++i) {
            queue->push(frames, true);
        }
        QElapsedTimer stopTimer;
        stopTimer.start();
        stopFlag.store(true);
        queue->wakeAll();
        const bool stoppedPromptly = worker.wait(500);
        if (!stoppedPromptly || stopTimer.elapsed() >= 500) {
            std::cerr << "SorterWorker stop latency would block page switching" << std::endl;
            return 4;
        }
        if (!ok) {
            std::cerr << "SorterWorker did not fill DataStore buffers" << std::endl;
            return 4;
        }
    }

    QVector<double> channel0Volts;
    {
        auto *buf = dataStore.buffer(0);
        const QVector<qint32> raw = buf->getLatest(kFrames).second;
        channel0Volts.reserve(raw.size());
        for (qint32 v : raw) {
            channel0Volts.push_back(static_cast<double>(v) /
                                    static_cast<double>(1 << ccv2::kAdcBits) *
                                    ccv2::kVref);
        }
    }

    const ccv2::SndrResult sndr = ccv2::calSndr(channel0Volts, 20000.0, 10000.0, QStringLiteral("hann"));
    if (!sndr.ok || !std::isfinite(sndr.fin) || sndr.fftData.isEmpty()) {
        std::cerr << "SNDR/FFT calculation failed" << std::endl;
        return 5;
    }

    const QString outDir = QStringLiteral("E:/BMI/C_code/analysis");
    QDir().mkpath(outDir);

    ccv2::WaveformWidget single(QStringLiteral("single channel smoke"));
    single.setData(channel0Volts.mid(0, 1024));
    if (!renderWidget(single, outDir + QStringLiteral("/ccv2_ui_pipeline_single.png"))) {
        std::cerr << "Single waveform render failed" << std::endl;
        return 4;
    }

    QVector<ccv2::WaveformWidget::PlotSeries> series;
    for (int i = 0; i < channels.size(); ++i) {
        auto *buf = dataStore.buffer(channels[i]);
        const QVector<qint32> raw = buf->getLatest(1024).second;
        ccv2::WaveformWidget::PlotSeries s;
        s.color = QColor::fromHsv((i * 90) % 360, 220, 255);
        for (qint32 v : raw) {
            s.data.push_back(static_cast<double>(v) / static_cast<double>(1 << ccv2::kAdcBits) * ccv2::kVref);
        }
        series.push_back(s);
    }

    ccv2::WaveformWidget multi(QStringLiteral("multi channel smoke"));
    multi.setSeries(series);
    if (!renderWidget(multi, outDir + QStringLiteral("/ccv2_ui_pipeline_multi.png"))) {
        std::cerr << "Multi waveform render failed" << std::endl;
        return 5;
    }

    ccv2::WaveformWidget fft(QStringLiteral("fft smoke"));
    QVector<double> fftDb;
    fftDb.reserve(sndr.fftData.size());
    for (double p : sndr.fftData) {
        fftDb.push_back(10.0 * std::log10(std::max(p, 1e-18)));
    }
    fft.setYRange(-180.0, 20.0);
    fft.setData(fftDb);
    if (!renderWidget(fft, outDir + QStringLiteral("/ccv2_ui_pipeline_fft.png"))) {
        std::cerr << "FFT waveform render failed" << std::endl;
        return 6;
    }

    const QString adcFile = outDir + QStringLiteral("/ADC_DATA_export_smoke.bin");
    {
        QFile adc(adcFile);
        if (!adc.open(QIODevice::WriteOnly | QIODevice::Truncate) || adc.write(frames) != frames.size()) {
            std::cerr << "Failed to create ADC export smoke input" << std::endl;
            return 7;
        }
    }

    const QString exportDirPath = outDir + QStringLiteral("/matlab_export_smoke");
    QDir exportDir(exportDirPath);
    if (exportDir.exists()) {
        exportDir.removeRecursively();
    }
    const ccv2::MatlabChannelExportResult exportResult =
        ccv2::MatlabChannelExporter::exportAdcBin(adcFile, exportDirPath, 20000.0);
    if (!exportResult.ok || exportResult.frameCount != kFrames) {
        std::cerr << "MATLAB channel export failed: "
                  << exportResult.error.toStdString() << std::endl;
        return 8;
    }

    QFile ch5(exportDirPath + QStringLiteral("/ch005_i32le.bin"));
    if (!ch5.open(QIODevice::ReadOnly) || ch5.size() != kFrames * ccv2::kBytesPerPoint) {
        std::cerr << "Exported channel file missing or wrong size" << std::endl;
        return 9;
    }
    const QByteArray ch5Bytes = ch5.readAll();
    for (int f : {0, 1, kFrames - 1}) {
        const qint32 expected = static_cast<qint32>(qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(frames.constData() + f * ccv2::kFrameBytes + 5 * ccv2::kBytesPerPoint)) &
            ccv2::kAdcSampleMask);
        const qint32 actual = qFromLittleEndian<qint32>(
            reinterpret_cast<const uchar *>(ch5Bytes.constData() + f * ccv2::kBytesPerPoint));
        if (actual != expected) {
            std::cerr << "Exported channel value mismatch" << std::endl;
            return 10;
        }
    }
    QFile loader(exportDirPath + QStringLiteral("/load_adc_channels.m"));
    QFile reader(exportDirPath + QStringLiteral("/read_adc_channel.m"));
    if (!loader.open(QIODevice::ReadOnly) || !reader.open(QIODevice::ReadOnly)) {
        std::cerr << "MATLAB loader scripts missing" << std::endl;
        return 11;
    }
    const QByteArray loaderText = loader.readAll();
    const QByteArray readerText = reader.readAll();
    if (!loaderText.contains("sprintf('ch%03d', ch)") ||
        !readerText.contains("sprintf('ch%03d_i32le.bin', ch)")) {
        std::cerr << "MATLAB loader script channel naming is invalid" << std::endl;
        return 12;
    }

    const QString raw32ExportDirPath = outDir + QStringLiteral("/matlab_raw32_export_smoke");
    QDir raw32ExportDir(raw32ExportDirPath);
    if (raw32ExportDir.exists()) raw32ExportDir.removeRecursively();
    ccv2::MatlabChannelExportOptions raw32Options;
    raw32Options.mode = ccv2::MatlabExportMode::RawWords;
    const ccv2::MatlabChannelExportResult raw32Result =
        ccv2::MatlabChannelExporter::exportAdcBin(
            adcFile, raw32ExportDirPath, 20000.0, raw32Options);
    QFile raw32Ch5(raw32ExportDirPath + QStringLiteral("/ch005_raw32le.bin"));
    if (!raw32Result.ok || !raw32Ch5.open(QIODevice::ReadOnly)) {
        std::cerr << "RAW32 export failed" << std::endl;
        return 13;
    }
    const QByteArray raw32Bytes = raw32Ch5.read(4);
    const quint32 raw32Expected = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(frames.constData() + 5 * ccv2::kBytesPerPoint));
    const quint32 raw32Actual = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(raw32Bytes.constData()));
    if (raw32Actual != raw32Expected) {
        std::cerr << "RAW32 export stripped timestamp bits" << std::endl;
        return 14;
    }
    QFile raw32Reader(raw32ExportDirPath + QStringLiteral("/read_adc_channel.m"));
    if (!raw32Reader.open(QIODevice::ReadOnly) ||
        !raw32Reader.readAll().contains("uint32=>uint32")) {
        std::cerr << "RAW32 MATLAB reader uses the wrong sample type" << std::endl;
        return 15;
    }
    std::atomic_bool exportCancelled{true};
    ccv2::MatlabChannelExportOptions cancelledOptions;
    cancelledOptions.cancelFlag = &exportCancelled;
    const ccv2::MatlabChannelExportResult cancelledResult =
        ccv2::MatlabChannelExporter::exportAdcBin(
            adcFile,
            outDir + QStringLiteral("/cancelled_export_smoke"),
            20000.0,
            cancelledOptions);
    if (cancelledResult.ok || !cancelledResult.error.contains(QStringLiteral("取消"))) {
        std::cerr << "Export cancellation was ignored" << std::endl;
        return 16;
    }

    const QString tdmExportDirPath = outDir + QStringLiteral("/matlab_tdm_export_smoke");
    QDir tdmExportDir(tdmExportDirPath);
    if (tdmExportDir.exists()) {
        tdmExportDir.removeRecursively();
    }
    ccv2::MatlabChannelExportOptions tdmOptions;
    tdmOptions.mode = ccv2::MatlabExportMode::TdmDemux;
    tdmOptions.evenSampleIsFirstLocalEle = true;
    const ccv2::MatlabChannelExportResult tdmExportResult =
        ccv2::MatlabChannelExporter::exportAdcBin(adcFile, tdmExportDirPath, 20000.0, tdmOptions);
    if (!tdmExportResult.ok || tdmExportResult.frameCount != kFrames || tdmExportResult.outputCount != 1024) {
        std::cerr << "TDM MATLAB export failed: "
                  << tdmExportResult.error.toStdString() << std::endl;
        return 13;
    }

    const int tdmSamples = kFrames / ccv2::kTdmPhaseCount;
    QFile ele20(tdmExportDirPath + QStringLiteral("/ele0020_i32le.bin"));
    QFile ele22(tdmExportDirPath + QStringLiteral("/ele0022_i32le.bin"));
    if (!ele20.open(QIODevice::ReadOnly) || ele20.size() != tdmSamples * ccv2::kBytesPerPoint ||
        !ele22.open(QIODevice::ReadOnly) || ele22.size() != tdmSamples * ccv2::kBytesPerPoint) {
        std::cerr << "TDM exported electrode files missing or wrong size" << std::endl;
        return 14;
    }
    const QByteArray ele20Bytes = ele20.readAll();
    const QByteArray ele22Bytes = ele22.readAll();
    for (int sample : {0, 1, tdmSamples - 1}) {
        const int phase0Frame = sample * ccv2::kTdmPhaseCount;
        const int phase2Frame = sample * ccv2::kTdmPhaseCount + 2;
        const qint32 expectedEven = static_cast<qint32>(qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(frames.constData() + phase0Frame * ccv2::kFrameBytes + 5 * ccv2::kBytesPerPoint)) &
            ccv2::kAdcSampleMask);
        const qint32 expectedOdd = static_cast<qint32>(qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(frames.constData() + phase2Frame * ccv2::kFrameBytes + 5 * ccv2::kBytesPerPoint)) &
            ccv2::kAdcSampleMask);
        const qint32 actualEven = qFromLittleEndian<qint32>(
            reinterpret_cast<const uchar *>(ele20Bytes.constData() + sample * ccv2::kBytesPerPoint));
        const qint32 actualOdd = qFromLittleEndian<qint32>(
            reinterpret_cast<const uchar *>(ele22Bytes.constData() + sample * ccv2::kBytesPerPoint));
        if (actualEven != expectedEven || actualOdd != expectedOdd) {
            std::cerr << "TDM exported electrode value mismatch" << std::endl;
            return 15;
        }
    }
    QFile tdmMap(tdmExportDirPath + QStringLiteral("/tdm_map.tsv"));
    QFile tdmMetadata(tdmExportDirPath + QStringLiteral("/metadata.txt"));
    QFile tdmLoader(tdmExportDirPath + QStringLiteral("/load_tdm_ele_channels.m"));
    QFile tdmReader(tdmExportDirPath + QStringLiteral("/read_tdm_ele.m"));
    if (!tdmMap.open(QIODevice::ReadOnly) ||
        !tdmMetadata.open(QIODevice::ReadOnly) ||
        !tdmLoader.open(QIODevice::ReadOnly) ||
        !tdmReader.open(QIODevice::ReadOnly)) {
        std::cerr << "TDM MATLAB export helper files missing" << std::endl;
        return 16;
    }
    const QByteArray mapText = tdmMap.readAll();
    const QByteArray metadataText = tdmMetadata.readAll();
    const QByteArray tdmLoaderText = tdmLoader.readAll();
    const QByteArray tdmReaderText = tdmReader.readAll();
    if (!mapText.contains("20\t5\t0\t0\tele0020_i32le.bin") ||
        !mapText.contains("22\t5\t2\t2\tele0022_i32le.bin") ||
        !metadataText.contains("export_mode=tdm_demux") ||
        !metadataText.contains("file_pattern=ele####_i32le.bin") ||
        !tdmLoaderText.contains("load_tdm_ele_channels") ||
        !tdmReaderText.contains("sprintf('ele%04d_i32le.bin', ele)")) {
        std::cerr << "TDM MATLAB helper content is invalid" << std::endl;
        return 17;
    }

    const QString splitSessionPath = outDir + QStringLiteral("/split_export_session");
    QDir splitSession(splitSessionPath);
    if (splitSession.exists()) splitSession.removeRecursively();
    QDir().mkpath(splitSessionPath);
    const int splitFrame = 777;
    const QByteArray firstPart = frames.left(splitFrame * ccv2::kFrameBytes);
    const QByteArray secondPart = frames.mid(splitFrame * ccv2::kFrameBytes);
    QFile part1(splitSession.filePath(QStringLiteral("ADC_DATA_001.bin")));
    QFile part2(splitSession.filePath(QStringLiteral("ADC_DATA_002.bin")));
    if (!part1.open(QIODevice::WriteOnly) || part1.write(firstPart) != firstPart.size() ||
        !part2.open(QIODevice::WriteOnly) || part2.write(secondPart) != secondPart.size()) {
        std::cerr << "Failed to create split session" << std::endl;
        return 18;
    }
    part1.close();
    part2.close();
    ccv2::SessionManifestData manifest;
    manifest.metadata.sampleRate = 20000.0;
    manifest.parts = {{QStringLiteral("ADC_DATA_001.bin"), firstPart.size(), splitFrame},
                      {QStringLiteral("ADC_DATA_002.bin"), secondPart.size(), kFrames - splitFrame}};
    manifest.totalBytes = frames.size();
    manifest.totalFrames = kFrames;
    manifest.complete = true;
    QString manifestError;
    if (!ccv2::SessionManifest::write(splitSessionPath, manifest, &manifestError)) {
        std::cerr << "Failed to write split manifest: " << manifestError.toStdString() << std::endl;
        return 19;
    }
    const QString splitExportPath = outDir + QStringLiteral("/split_export_output");
    QDir splitOutput(splitExportPath);
    if (splitOutput.exists()) splitOutput.removeRecursively();
    const ccv2::MatlabChannelExportResult splitResult =
        ccv2::MatlabChannelExporter::exportAdcBin(
            splitSession.filePath(QStringLiteral("ADC_DATA_001.bin")),
            splitExportPath,
            20000.0);
    if (!splitResult.ok || splitResult.frameCount != kFrames) {
        std::cerr << "Split-session export did not include every part: "
                  << splitResult.error.toStdString() << std::endl;
        return 20;
    }

    std::cout << "ui_pipeline_smoke ok fin=" << sndr.fin
              << " fft_bins=" << sndr.fftData.size() << std::endl;
    return 0;
}
