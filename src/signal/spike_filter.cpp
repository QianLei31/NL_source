#include "signal/spike_filter.h"

#include <algorithm>
#include <cmath>

namespace ccv2 {

namespace {
constexpr double kPi = 3.14159265358979323846;

double boundedFrequency(double frequency, double sampleRate)
{
    const double fs = std::max(1.0, sampleRate);
    return std::clamp(frequency, 0.1, fs * 0.49);
}
}

Biquad Biquad::highpass(double fc, double fs, double q) {
    Biquad bq;
    const double safeFs = std::max(1.0, fs);
    const double w0 = 2.0 * kPi * boundedFrequency(fc, safeFs) / safeFs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * std::max(1e-6, q));
    const double a0 = 1.0 + alpha;
    bq.b0 = (1.0 + cw) / 2.0 / a0;
    bq.b1 = -(1.0 + cw) / a0;
    bq.b2 = (1.0 + cw) / 2.0 / a0;
    bq.a1 = (-2.0 * cw) / a0;
    bq.a2 = (1.0 - alpha) / a0;
    return bq;
}

Biquad Biquad::lowpass(double fc, double fs, double q) {
    Biquad bq;
    const double safeFs = std::max(1.0, fs);
    const double w0 = 2.0 * kPi * boundedFrequency(fc, safeFs) / safeFs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * std::max(1e-6, q));
    const double a0 = 1.0 + alpha;
    bq.b0 = (1.0 - cw) / 2.0 / a0;
    bq.b1 = (1.0 - cw) / a0;
    bq.b2 = (1.0 - cw) / 2.0 / a0;
    bq.a1 = (-2.0 * cw) / a0;
    bq.a2 = (1.0 - alpha) / a0;
    return bq;
}

namespace {
// Butterworth Q for section k of an N-th order cascade (N even).
double butterworthSectionQ(int k, int order) {
    return 1.0 / (2.0 * std::sin((2.0 * k + 1.0) * kPi / (2.0 * order)));
}
int evenOrder(int order) {
    // Clamp to a supported even order (2/4/6/8); round odd up.
    int o = std::clamp(order, 2, 8);
    if (o % 2 != 0) ++o;
    return std::min(o, 8);
}
}  // namespace

BiquadCascade BiquadCascade::butterworthHighpass(double fc, double fs, int order) {
    BiquadCascade c;
    const int o = evenOrder(order);
    for (int k = 0; k < o / 2; ++k) {
        c.sections.push_back(Biquad::highpass(fc, fs, butterworthSectionQ(k, o)));
    }
    return c;
}

BiquadCascade BiquadCascade::butterworthLowpass(double fc, double fs, int order) {
    BiquadCascade c;
    const int o = evenOrder(order);
    for (int k = 0; k < o / 2; ++k) {
        c.sections.push_back(Biquad::lowpass(fc, fs, butterworthSectionQ(k, o)));
    }
    return c;
}

Biquad Biquad::notch(double f0, double bandwidthHz, double fs) {
    Biquad bq;
    const double safeFs = std::max(1.0, fs);
    const double safeF0 = boundedFrequency(f0, safeFs);
    const double w0 = 2.0 * kPi * safeF0 / safeFs;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double q = std::max(0.1, safeF0 / std::max(0.1, bandwidthHz));
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    bq.b0 = 1.0 / a0;
    bq.b1 = (-2.0 * cw) / a0;
    bq.b2 = 1.0 / a0;
    bq.a1 = (-2.0 * cw) / a0;
    bq.a2 = (1.0 - alpha) / a0;
    return bq;
}

void SpikeProcessor::configure(int numChannels, const SpikeProcConfig &cfg) {
    m_cfg = cfg;
    const double fs = std::max(1.0, cfg.sampleRate);
    m_refractorySamples = std::max(1, static_cast<int>(cfg.refractoryMs / 1000.0 * fs));
    // RMS estimator: exponential moving average of x^2 with ~250 ms time constant.
    const double tau = 0.25;
    m_rmsWarmupSamples = std::max(32, static_cast<int>(tau * fs));
    m_rmsAlpha = std::clamp(1.0 / (tau * fs), 1e-5, 1.0);

    const int order = cfg.filterOrder;
    m_ch.resize(numChannels);
    for (ChState &ch : m_ch) {
        ch.notch = Biquad::notch(cfg.notchHz > 0 ? cfg.notchHz : 50.0, 10.0, fs);
        ch.hp = BiquadCascade::butterworthHighpass(cfg.highpassHz, fs, order);
        ch.spikeLp = BiquadCascade::butterworthLowpass(cfg.spikeLowpassHz, fs, order);
        ch.lfpLp = BiquadCascade::butterworthLowpass(cfg.lowpassHz, fs, order);
        ch.notch.reset();
        ch.hp.reset();
        ch.spikeLp.reset();
        ch.lfpLp.reset();
        ch.baseline = 0.0;
        ch.previousSpikeValue = 0.0;
        ch.rmsEma = 0.0;
        ch.rmsWarmupSumSq = 0.0;
        ch.baselineInit = false;
        ch.previousSpikeValid = false;
        ch.rmsInit = false;
        ch.thresholdOverrideEnabled = false;
        ch.thresholdOverrideV = 0.0;
        ch.rmsWarmupCount = 0;
        ch.refractory = 0;
    }
}

void SpikeProcessor::reset() {
    for (ChState &ch : m_ch) {
        ch.notch.reset();
        ch.hp.reset();
        ch.spikeLp.reset();
        ch.lfpLp.reset();
        ch.baseline = 0.0;
        ch.previousSpikeValue = 0.0;
        ch.rmsEma = 0.0;
        ch.rmsWarmupSumSq = 0.0;
        ch.baselineInit = false;
        ch.previousSpikeValid = false;
        ch.rmsInit = false;
        ch.rmsWarmupCount = 0;
        ch.refractory = 0;
    }
}

double SpikeProcessor::currentRms(int idx) const {
    if (idx < 0 || idx >= m_ch.size()) return 0.0;
    return std::sqrt(std::max(0.0, m_ch[idx].rmsEma));
}

double SpikeProcessor::currentThreshold(int idx) const {
    if (idx < 0 || idx >= m_ch.size()) return 0.0;
    if (m_ch[idx].thresholdOverrideEnabled) {
        return m_ch[idx].thresholdOverrideV;
    }
    if (m_cfg.absoluteThreshold) {
        return m_cfg.absThresholdV;
    }
    return (m_cfg.negativePolarity ? -1.0 : 1.0) * m_cfg.rmsMultiple * currentRms(idx);
}

bool SpikeProcessor::detectorReady(int idx) const {
    if (idx >= 0 && idx < m_ch.size() && m_ch[idx].thresholdOverrideEnabled) {
        return true;
    }
    if (m_cfg.absoluteThreshold) return true;
    if (idx < 0 || idx >= m_ch.size()) return false;
    return m_ch[idx].rmsInit;
}

void SpikeProcessor::setChannelThresholdOverride(int idx, bool enabled,
                                                 double thresholdV) {
    if (idx < 0 || idx >= m_ch.size()) return;
    m_ch[idx].thresholdOverrideEnabled = enabled;
    m_ch[idx].thresholdOverrideV = thresholdV;
}

void SpikeProcessor::processChannel(int idx, const QVector<double> &in,
                                    QVector<double> &disp, QVector<bool> &spikeFlags) {
    disp.resize(in.size());
    spikeFlags.resize(in.size());
    if (idx < 0 || idx >= m_ch.size()) {
        for (int i = 0; i < in.size(); ++i) { disp[i] = in[i]; spikeFlags[i] = false; }
        return;
    }
    ChState &ch = m_ch[idx];

    for (int i = 0; i < in.size(); ++i) {
        double x = in[i];
        if (!std::isfinite(x)) x = 0.0;

        if (!ch.baselineInit) {
            ch.baseline = x;
            ch.baselineInit = true;
        }
        const double centered = x - ch.baseline;
        const double nx = (m_cfg.notchHz > 0) ? ch.notch.process(centered) : centered;
        const double hpVal = ch.hp.process(nx);
        const double spikeVal = ch.spikeLp.process(hpVal);

        double dispVal;
        switch (m_cfg.band) {
        case SpikeProcConfig::Wideband: dispVal = nx + ch.baseline; break;
        case SpikeProcConfig::LfpBand:  dispVal = ch.lfpLp.process(nx); break;
        default:                        dispVal = spikeVal; break;
        }
        disp[i] = dispVal;

        // Update running RMS on the spike band.
        const double sq = spikeVal * spikeVal;
        if (!ch.rmsInit) {
            ch.rmsWarmupSumSq += sq;
            ++ch.rmsWarmupCount;
            ch.rmsEma = ch.rmsWarmupSumSq /
                        static_cast<double>(ch.rmsWarmupCount);
            if (ch.rmsWarmupCount >= m_rmsWarmupSamples) {
                ch.rmsInit = true;
            }
        } else {
            ch.rmsEma += m_rmsAlpha * (sq - ch.rmsEma);
        }
        const double rms = std::sqrt(std::max(0.0, ch.rmsEma));

        double threshold = m_cfg.absThresholdV;
        if (ch.thresholdOverrideEnabled) {
            threshold = ch.thresholdOverrideV;
        } else if (m_cfg.absoluteThreshold) {
            threshold = m_cfg.absThresholdV;
        } else {
            threshold = (m_cfg.negativePolarity ? -1.0 : 1.0) * m_cfg.rmsMultiple * rms;
        }

        bool spike = false;
        const bool detectorReady =
            ch.thresholdOverrideEnabled || m_cfg.absoluteThreshold || ch.rmsInit;
        if (ch.refractory > 0) {
            --ch.refractory;
        } else if (detectorReady) {
            const bool cross =
                ch.previousSpikeValid &&
                (m_cfg.negativePolarity
                     ? (ch.previousSpikeValue > threshold && spikeVal <= threshold)
                     : (ch.previousSpikeValue < threshold && spikeVal >= threshold));
            if (cross) {
                spike = true;
                ch.refractory = m_refractorySamples;
            }
        }
        spikeFlags[i] = spike;
        ch.previousSpikeValue = spikeVal;
        ch.previousSpikeValid = true;
    }
}

}  // namespace ccv2
