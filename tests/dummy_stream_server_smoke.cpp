#include "core/threadsafe_queue.h"
#include "network/data_sorter.h"
#include "network/socket_receiver2.h"
#include "network/timestamp_checker.h"
#include "service/dummy_stream_server.h"
#include "service/spike_detect_worker.h"
#include "signal/spike_snippet_store.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>

namespace {

int availablePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return static_cast<int>(server.serverPort());
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
        QThread::msleep(5);
    }
    return predicate();
}

bool runCase(int controlPort, int dataPort)
{
    ccv2::DummyStreamServer server;
    QString startError;
    if (!server.start(controlPort, dataPort, &startError)) {
        std::cerr << startError.toStdString() << std::endl;
        return false;
    }

    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(128);
    auto state = std::make_shared<ccv2::RealtimeStreamState>();
    {
        QMutexLocker locker(&state->lock);
        state->channels = {0, 8, 31};
        state->maxSamples = 8192;
    }

    std::atomic_bool stopFlag{false};
    std::unique_ptr<ccv2::SocketReceiver2> receiver;
    if (controlPort == dataPort) {
        receiver = std::make_unique<ccv2::SocketReceiver2>(
            QStringLiteral("127.0.0.1"), controlPort, queue, &stopFlag);
    } else {
        receiver = std::make_unique<ccv2::SocketReceiver2>(
            QStringLiteral("127.0.0.1"), controlPort, dataPort, queue, &stopFlag);
    }
    receiver->setCommand(QStringLiteral("ctre"));

    QString error;
    QObject::connect(receiver.get(), &ccv2::SocketReceiver2::connectionError,
                     [&](const QString &message) { error = message; });

    ccv2::DataSorter sorter(queue, state, &stopFlag);
    sorter.start();
    receiver->start();

    const bool received = waitFor([&]() {
        QMutexLocker locker(&state->lock);
        return state->buffers.value(0).size() >= 4096;
    }, 5000);

    QVector<double> channel0;
    QVector<ccv2::RealtimeChannelMetric> metrics;
    if (received) {
        QMutexLocker locker(&state->lock);
        const QQueue<double> &samples = state->buffers.value(0);
        channel0.reserve(samples.size());
        for (double sample : samples) {
            channel0.push_back(sample);
        }
        metrics = state->metrics;
    }

    stopFlag.store(true);
    queue->wakeAll();
    receiver->wait(2000);
    sorter.wait(2000);

    if (!received || channel0.size() < 4096) {
        std::cerr << "no stream data: " << error.toStdString() << std::endl;
        return false;
    }

    for (const auto &metric : metrics) {
        if (metric.valid &&
            (metric.mean < 0.0 || metric.mean > 1.8 ||
             metric.rms < 0.0 || metric.rms > 1.8 ||
             metric.p2p < 0.0 || metric.p2p > 1.8)) {
            std::cerr << "metric outside ADC range" << std::endl;
            return false;
        }
    }

    double mean = 0.0;
    for (double value : channel0) {
        mean += value;
    }
    mean /= channel0.size();

    QVector<double> crossings;
    double maxStep = 0.0;
    for (int i = 1; i < channel0.size(); ++i) {
        maxStep = std::max(maxStep, std::abs(channel0[i] - channel0[i - 1]));
        const double y0 = channel0[i - 1] - mean;
        const double y1 = channel0[i] - mean;
        if (y0 <= 0.0 && y1 > 0.0) {
            crossings.push_back(i - 1 - y0 / (y1 - y0));
        }
    }

    if (crossings.size() < 3) {
        return false;
    }
    const double measuredHz =
        (crossings.size() - 1) * ccv2::DummyStreamServer::kSampleRate /
        (crossings.last() - crossings.first());
    if (std::abs(measuredHz - ccv2::DummyStreamServer::kBaseFrequencyHz) > 1.0) {
        std::cerr << "frequency=" << measuredHz << std::endl;
        return false;
    }
    if (maxStep > 0.02) {
        std::cerr << "discontinuity=" << maxStep << std::endl;
        return false;
    }
    return true;
}

bool runSpikeCase(int port)
{
    ccv2::DummyStreamServer server;
    server.setWaveform(ccv2::DummyWaveform::Spike);
    QString startError;
    if (!server.start(port, port, &startError)) {
        std::cerr << startError.toStdString() << std::endl;
        return false;
    }

    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(128);
    auto state = std::make_shared<ccv2::RealtimeStreamState>();
    {
        QMutexLocker locker(&state->lock);
        state->channels = {1, 11};  // deterministic silent and active channels
        state->maxSamples = 12000;
    }

    std::atomic_bool stopFlag{false};
    ccv2::SocketReceiver2 receiver(
        QStringLiteral("127.0.0.1"), port, queue, &stopFlag);
    receiver.setCommand(QStringLiteral("ctre"));
    ccv2::DataSorter sorter(queue, state, &stopFlag);
    sorter.start();
    receiver.start();

    const bool received = waitFor([&]() {
        QMutexLocker locker(&state->lock);
        return state->buffers.value(11).size() >= 8192;
    }, 5000);

    QVector<double> silentSamples;
    QVector<double> activeSamples;
    if (received) {
        QMutexLocker locker(&state->lock);
        const QQueue<double> &silentBuffer = state->buffers.value(1);
        const QQueue<double> &activeBuffer = state->buffers.value(11);
        silentSamples.reserve(silentBuffer.size());
        activeSamples.reserve(activeBuffer.size());
        for (double value : silentBuffer) silentSamples.push_back(value);
        for (double value : activeBuffer) activeSamples.push_back(value);
    }

    stopFlag.store(true);
    queue->wakeAll();
    receiver.wait(2000);
    sorter.wait(2000);
    if (!received || silentSamples.size() < 8192 || activeSamples.size() < 8192) {
        return false;
    }

    const auto silentMinmax =
        std::minmax_element(silentSamples.cbegin(), silentSamples.cend());
    const auto activeMinmax =
        std::minmax_element(activeSamples.cbegin(), activeSamples.cend());
    const double silentP2p = *silentMinmax.second - *silentMinmax.first;
    const double activeP2p = *activeMinmax.second - *activeMinmax.first;
    double mean = 0.0;
    for (double value : activeSamples) mean += value;
    mean /= activeSamples.size();
    // The built-in source models 80-420 uV input spikes through the default
    // 60x REC gain, so active channels should span tens of millivolts at the
    // ADC while remaining clearly separated from the input-noise floor.
    if (activeP2p <= 0.012 || activeP2p <= silentP2p + 0.008) {
        std::cerr << "spike separation active=" << activeP2p
                  << " silent=" << silentP2p << std::endl;
        return false;
    }
    if (silentP2p >= 0.01) {
        std::cerr << "silent channel too noisy: " << silentP2p << std::endl;
        return false;
    }
    return mean > 0.7 && mean < 1.1;
}

bool runTimestampCase(int port)
{
    ccv2::DummyStreamServer server;
    QString startError;
    if (!server.start(port, port, &startError)) {
        std::cerr << startError.toStdString() << std::endl;
        return false;
    }

    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(64);
    std::atomic_bool stopFlag{false};
    ccv2::SocketReceiver2 receiver(
        QStringLiteral("127.0.0.1"), port, queue, &stopFlag);
    receiver.setCommand(QStringLiteral("ctre"));
    receiver.start();

    ccv2::TimestampContinuityAnalyzer analyzer(1);
    const bool received = waitFor([&]() {
        QByteArray chunk;
        while (queue->pop(chunk, 0)) {
            analyzer.process(chunk);
        }
        return analyzer.stats().framesSeen >= 1000;
    }, 5000);

    stopFlag.store(true);
    queue->wakeAll();
    receiver.wait(2000);
    const auto stats = analyzer.stats();
    return received &&
           stats.framesSeen >= 1000 &&
           stats.discontinuities == 0 &&
           stats.repeatedFrames == 0 &&
           stats.intraFrameMismatchFrames == 0 &&
           stats.firstTimestamp == 0u;
}

bool runSpikeDetectorCase(int port)
{
    ccv2::DummyStreamServer server;
    server.setWaveform(ccv2::DummyWaveform::Spike);
    QString startError;
    if (!server.start(port, port, &startError)) {
        std::cerr << startError.toStdString() << std::endl;
        return false;
    }

    auto queue = std::make_shared<ccv2::ThreadSafeQueue<QByteArray>>(256);
    std::atomic_bool stopFlag{false};
    ccv2::SpikeDetectConfig cfg;
    cfg.proc.sampleRate = ccv2::DummyStreamServer::kSampleRate;
    cfg.proc.rmsMultiple = 4.0;
    cfg.proc.negativePolarity = true;
    cfg.inputGain = 60.0;
    cfg.preSamples = 8;
    cfg.postSamples = 24;

    ccv2::SpikeSnippetStore store;
    store.configure(ccv2::DummyStreamServer::kChannels,
                    cfg.preSamples + cfg.postSamples + 1,
                    64);
    ccv2::SpikeDetectWorker worker(queue, &store, &stopFlag, cfg);
    std::atomic<qint64> processed{0};
    QObject::connect(&worker, &ccv2::SpikeDetectWorker::framesProcessed,
                     [&processed](qint64 frames) { processed.fetch_add(frames); });

    ccv2::SocketReceiver2 receiver(
        QStringLiteral("127.0.0.1"), port, queue, &stopFlag);
    receiver.setCommand(QStringLiteral("ctre"));
    worker.start();
    receiver.start();

    const bool enoughData = waitFor(
        [&processed]() { return processed.load() >= 50000; }, 8000);

    stopFlag.store(true);
    queue->wakeAll();
    receiver.wait(2000);
    worker.wait(2000);

    QVector<qint64> totals;
    store.snapshotStats(&totals, nullptr, nullptr);
    const qint64 silentCount = totals.value(1);
    const qint64 activeCount = totals.value(11);
    if (!enoughData || activeCount < 5 || activeCount <= silentCount + 3) {
        std::cerr << "detector separation frames=" << processed.load()
                  << " active=" << activeCount
                  << " silent=" << silentCount << std::endl;
        return false;
    }
    QVector<float> snippets;
    quint64 sequence = 0;
    if (store.fetchAll(11, &snippets, &sequence) <= 0) {
        std::cerr << "detector produced no retained active-channel snippet"
                  << std::endl;
        return false;
    }
    double peakInputV = 0.0;
    for (float sample : snippets) {
        peakInputV = std::max(peakInputV, std::abs(static_cast<double>(sample)));
    }
    if (peakInputV < 20e-6 || peakInputV > 1e-3) {
        std::cerr << "input-referred spike amplitude out of range: "
                  << peakInputV * 1e6 << " uV" << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int singlePort = availablePort();
    const int controlPort = availablePort();
    const int dataPort = availablePort();
    const int spikePort = availablePort();
    const int timestampPort = availablePort();
    const int spikeDetectorPort = availablePort();
    if (!singlePort || !controlPort || !dataPort || !spikePort ||
        !timestampPort || !spikeDetectorPort ||
        controlPort == dataPort) {
        return 1;
    }
    if (!runCase(singlePort, singlePort)) {
        return 2;
    }
    if (!runCase(controlPort, dataPort)) {
        return 3;
    }
    if (!runSpikeCase(spikePort)) {
        return 4;
    }
    if (!runTimestampCase(timestampPort)) {
        return 5;
    }
    if (!runSpikeDetectorCase(spikeDetectorPort)) {
        return 6;
    }
    std::cout << "dummy_stream_server_smoke ok" << std::endl;
    return 0;
}
