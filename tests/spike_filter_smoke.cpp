#include "signal/spike_filter.h"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

bool allFinite(const QVector<double> &values)
{
    for (double value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

}  // namespace

int main()
{
    constexpr double kFs = 20000.0;

    {
        ccv2::SpikeProcConfig cfg;
        cfg.sampleRate = kFs;
        cfg.notchHz = 0;
        ccv2::SpikeProcessor processor;
        processor.configure(1, cfg);

        QVector<double> input(5000, 0.9);
        QVector<double> output;
        QVector<bool> flags;
        processor.processChannel(0, input, output, flags);

        double maxAbs = 0.0;
        int detections = 0;
        for (int i = 0; i < output.size(); ++i) {
            maxAbs = std::max(maxAbs, std::abs(output[i]));
            if (flags.value(i)) ++detections;
        }
        if (!allFinite(output) || maxAbs > 1e-12 || detections != 0) {
            std::cerr << "DC baseline caused a spike-filter startup transient"
                      << std::endl;
            return 1;
        }
    }

    {
        ccv2::SpikeProcConfig cfg;
        cfg.sampleRate = kFs;
        cfg.notchHz = 0;
        cfg.band = ccv2::SpikeProcConfig::LfpBand;
        ccv2::SpikeProcessor processor;
        processor.configure(1, cfg);

        QVector<double> input;
        input.reserve(4000);
        for (int i = 0; i < 4000; ++i) {
            input.push_back(0.9 + 0.04 * std::sin(2.0 * 3.14159265358979323846 *
                                                 10.0 * i / kFs));
        }
        QVector<double> output;
        QVector<bool> flags;
        processor.processChannel(0, input, output, flags);

        double maxAbs = 0.0;
        for (double value : output) {
            maxAbs = std::max(maxAbs, std::abs(value));
        }
        if (!allFinite(output) || maxAbs > 0.08) {
            std::cerr << "LFP output retained the ADC common-mode offset"
                      << std::endl;
            return 2;
        }
    }

    {
        ccv2::SpikeProcConfig cfg;
        cfg.sampleRate = kFs;
        cfg.notchHz = 0;
        cfg.absoluteThreshold = true;
        cfg.absThresholdV = -0.01;
        cfg.negativePolarity = true;
        cfg.refractoryMs = 1.0;
        ccv2::SpikeProcessor processor;
        processor.configure(1, cfg);

        QVector<double> input(500, 0.9);
        for (int i = 100; i < input.size(); ++i) {
            input[i] = 0.82;
        }
        QVector<double> output;
        QVector<bool> flags;
        processor.processChannel(0, input, output, flags);

        int detections = 0;
        for (bool flag : flags) {
            if (flag) ++detections;
        }
        if (!allFinite(output) || detections != 1) {
            std::cerr << "threshold crossing generated " << detections
                      << " detections for one sustained event" << std::endl;
            return 3;
        }
    }

    {
        ccv2::SpikeProcConfig cfg;
        cfg.sampleRate = kFs;
        cfg.notchHz = 0;
        cfg.absoluteThreshold = false;
        cfg.rmsMultiple = 4.0;
        ccv2::SpikeProcessor processor;
        processor.configure(1, cfg);

        QVector<double> input;
        input.reserve(7000);
        for (int i = 0; i < 7000; ++i) {
            double value =
                0.9 + 0.001 * std::sin(2.0 * 3.14159265358979323846 *
                                       100.0 * i / kFs);
            if (i >= 6000 && i < 6010) value -= 0.08;
            input.push_back(value);
        }
        QVector<double> output;
        QVector<bool> flags;
        processor.processChannel(0, input, output, flags);

        int warmupDetections = 0;
        int postWarmupDetections = 0;
        for (int i = 0; i < flags.size(); ++i) {
            if (!flags[i]) continue;
            if (i < 5000) ++warmupDetections;
            else ++postWarmupDetections;
        }
        if (warmupDetections != 0 || postWarmupDetections < 1) {
            std::cerr << "RMS warmup/detection behavior is invalid"
                      << std::endl;
            return 4;
        }
    }

    {
        ccv2::Biquad invalidCutoff =
            ccv2::Biquad::highpass(5000.0, 2000.0);
        double value = 0.0;
        for (int i = 0; i < 10000; ++i) {
            value = invalidCutoff.process(i == 0 ? 1.0 : 0.0);
            if (!std::isfinite(value)) {
                std::cerr << "out-of-Nyquist cutoff produced non-finite output"
                          << std::endl;
                return 5;
            }
        }
    }

    {
        ccv2::SpikeProcConfig cfg;
        cfg.sampleRate = kFs;
        cfg.notchHz = 0;
        cfg.absoluteThreshold = true;
        cfg.absThresholdV = -0.2;
        cfg.negativePolarity = true;
        ccv2::SpikeProcessor processor;
        processor.configure(2, cfg);
        processor.setChannelThresholdOverride(0, true, -0.01);

        QVector<double> input(500, 0.9);
        for (int i = 100; i < input.size(); ++i) input[i] = 0.82;
        QVector<double> output;
        QVector<bool> lane0Flags;
        QVector<bool> lane1Flags;
        processor.processChannel(0, input, output, lane0Flags);
        processor.processChannel(1, input, output, lane1Flags);

        const int lane0Detections =
            std::count(lane0Flags.cbegin(), lane0Flags.cend(), true);
        const int lane1Detections =
            std::count(lane1Flags.cbegin(), lane1Flags.cend(), true);
        if (lane0Detections != 1 || lane1Detections != 0 ||
            std::abs(processor.currentThreshold(0) + 0.01) > 1e-12 ||
            std::abs(processor.currentThreshold(1) + 0.2) > 1e-12) {
            std::cerr << "per-channel threshold override affected the wrong lane"
                      << std::endl;
            return 6;
        }

        processor.setChannelThresholdOverride(0, false, 0.0);
        if (std::abs(processor.currentThreshold(0) + 0.2) > 1e-12) {
            std::cerr << "disabled threshold override did not restore global value"
                      << std::endl;
            return 7;
        }
    }

    return 0;
}
