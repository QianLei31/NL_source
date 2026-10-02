#pragma once

#include <QVector>

namespace ccv2 {

// Direct-form-II transposed biquad (RBJ cookbook coefficients).
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = 0.0; z2 = 0.0; }
    inline double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    static Biquad highpass(double fc, double fs, double q = 0.70710678);
    static Biquad lowpass(double fc, double fs, double q = 0.70710678);
    static Biquad notch(double f0, double bandwidthHz, double fs);
};

// Cascade of second-order sections realising a higher-order Butterworth
// response (N even). Each section uses the Butterworth pole Q so the whole
// cascade rolls off at N*6 dB/oct — much steeper band separation than a single
// biquad's 12 dB/oct, matching Intan's high-order filters.
struct BiquadCascade {
    QVector<Biquad> sections;

    void reset() { for (Biquad &s : sections) s.reset(); }
    inline double process(double x) {
        for (Biquad &s : sections) x = s.process(x);
        return x;
    }

    static BiquadCascade butterworthHighpass(double fc, double fs, int order);
    static BiquadCascade butterworthLowpass(double fc, double fs, int order);
};

// Per-channel spike processing modeled on Intan-RHX: notch + band filter
// (WIDE/SPIKE/LFP) and threshold spike detection (absolute or RMS-multiple,
// selectable polarity, refractory period). Detection always runs on the
// highpass (spike) band regardless of the displayed band.
struct SpikeProcConfig {
    enum Band { Wideband = 0, SpikeBand = 1, LfpBand = 2 };
    int band = SpikeBand;
    // 2/4/6/8. Higher = steeper band edges, but Butterworth ringing on sharp
    // transients can double-trigger detection, so detection defaults to 2.
    int filterOrder = 2;
    int notchHz = 50;            // 0 = off, else 50 / 60
    double highpassHz = 250.0;   // spike-band highpass cutoff
    double spikeLowpassHz = 6000.0;
    double lowpassHz = 300.0;    // LFP lowpass cutoff
    bool absoluteThreshold = false;
    double absThresholdV = -0.05;  // volts (sign carries polarity)
    double rmsMultiple = 4.0;
    bool negativePolarity = true;
    double refractoryMs = 1.0;
    double sampleRate = 20000.0;
};

class SpikeProcessor {
public:
    void configure(int numChannels, const SpikeProcConfig &cfg);
    void reset();

    // Process one channel's sample batch. Fills `disp` with the displayed band
    // and `spikeFlags` (one bool per input sample) with detection events.
    void processChannel(int idx, const QVector<double> &in,
                        QVector<double> &disp, QVector<bool> &spikeFlags);

    const SpikeProcConfig &config() const { return m_cfg; }
    int channels() const { return m_ch.size(); }

    // Detector state for display (threshold line, noise readout). Both are in
    // volts on the spike band; the threshold carries its polarity sign.
    double currentThreshold(int idx) const;
    double currentRms(int idx) const;
    bool detectorReady(int idx) const;
    // Override one channel with a signed absolute threshold. The detection
    // worker applies this on its own thread.
    void setChannelThresholdOverride(int idx, bool enabled, double thresholdV);

private:
    struct ChState {
        Biquad notch;
        BiquadCascade hp;
        BiquadCascade spikeLp;
        BiquadCascade lfpLp;
        double baseline = 0.0;
        double previousSpikeValue = 0.0;
        double rmsEma = 0.0;
        double rmsWarmupSumSq = 0.0;
        bool baselineInit = false;
        bool previousSpikeValid = false;
        bool rmsInit = false;
        bool thresholdOverrideEnabled = false;
        double thresholdOverrideV = 0.0;
        int rmsWarmupCount = 0;
        int refractory = 0;
    };
    QVector<ChState> m_ch;
    SpikeProcConfig m_cfg;
    int m_refractorySamples = 20;
    int m_rmsWarmupSamples = 5000;
    double m_rmsAlpha = 0.0;
};

}  // namespace ccv2
