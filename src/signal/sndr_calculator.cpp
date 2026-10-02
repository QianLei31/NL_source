#include "signal/sndr_calculator.h"

#include <QtMath>

#include <algorithm>
#include <complex>
#include <numeric>

namespace ccv2 {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isPowerOfTwo(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

double besselI0(double x) {
    const double absolute = std::abs(x);
    if (absolute < 3.75) {
        const double y = (x / 3.75) * (x / 3.75);
        return 1.0
               + y * (3.5156229
                      + y * (3.0899424
                             + y * (1.2067492
                                    + y * (0.2659732
                                           + y * (0.0360768 + y * 0.0045813)))));
    }

    const double y = 3.75 / absolute;
    return (std::exp(absolute) / std::sqrt(absolute))
           * (0.39894228
              + y * (0.01328592
                     + y * (0.00225319
                            + y * (-0.00157565
                                   + y * (0.00916281
                                          + y * (-0.02057706
                                                 + y * (0.02635537
                                                        + y * (-0.01647633
                                                               + y * 0.00392377))))))));
}

QString normalizedWindowName(const QString &type) {
    const QString name = type.trimmed().toLower();
    return name.isEmpty() ? QStringLiteral("hann") : name;
}

QVector<double> makeWindow(int n, const QString &type) {
    const QString name = normalizedWindowName(type);
    QVector<double> win(n, 1.0);
    if (n <= 1) {
        return win;
    }

    if (name == QStringLiteral("rect") || name == QStringLiteral("rectangle")) {
        return win;
    }
    if (name == QStringLiteral("hann")) {
        for (int i = 0; i < n; ++i) {
            win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        }
    } else if (name == QStringLiteral("blackman")) {
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
            win[i] = 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a);
        }
    } else if (name == QStringLiteral("blackmanharris")) {
        constexpr double a0 = 3.58750287312166e-001;
        constexpr double a1 = 4.88290107472600e-001;
        constexpr double a2 = 1.41279712970519e-001;
        constexpr double a3 = 1.16798922447150e-002;
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
            win[i] = a0 - a1 * std::cos(a) + a2 * std::cos(2.0 * a) - a3 * std::cos(3.0 * a);
        }
    } else if (name == QStringLiteral("kaiser")) {
        const double beta = 20.0;
        const double den = besselI0(beta);
        for (int i = 0; i < n; ++i) {
            const double r = (2.0 * i) / static_cast<double>(n - 1) - 1.0;
            const double val = std::sqrt(std::max(0.0, 1.0 - r * r));
            win[i] = besselI0(beta * val) / den;
        }
    }
    return win;
}

int defaultSignalBinsForWindow(const QString &window) {
    const QString name = normalizedWindowName(window);
    if (name == QStringLiteral("rect") || name == QStringLiteral("rectangle")) {
        return 1;
    }
    if (name == QStringLiteral("blackman") || name == QStringLiteral("blackmanharris") || name == QStringLiteral("kaiser")) {
        return 7;
    }
    return 5;
}

QVector<std::complex<double>> fftRadix2(QVector<std::complex<double>> a) {
    const int n = a.size();
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int j2 = 0; j2 < len / 2; ++j2) {
                const std::complex<double> u = a[i + j2];
                const std::complex<double> v = a[i + j2 + len / 2] * w;
                a[i + j2] = u + v;
                a[i + j2 + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    return a;
}

QVector<std::complex<double>> dft(const QVector<std::complex<double>> &in) {
    const int n = in.size();
    QVector<std::complex<double>> out(n);
    for (int k = 0; k < n; ++k) {
        std::complex<double> sum(0.0, 0.0);
        for (int t = 0; t < n; ++t) {
            const double ang = -2.0 * kPi * static_cast<double>(k) * static_cast<double>(t) / static_cast<double>(n);
            sum += in[t] * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        out[k] = sum;
    }
    return out;
}

QVector<std::complex<double>> fft(const QVector<std::complex<double>> &in) {
    if (isPowerOfTwo(in.size())) {
        return fftRadix2(in);
    }
    return dft(in);
}

double sumSlice(const QVector<double> &v, int start, int endExclusive) {
    if (v.isEmpty()) {
        return 0.0;
    }
    const int s = std::max(0, start);
    const int e = std::min(endExclusive, static_cast<int>(v.size()));
    if (e <= s) {
        return 0.0;
    }
    double sum = 0.0;
    for (int i = s; i < e; ++i) {
        sum += v[i];
    }
    return sum;
}

double clampPositive(double v) {
    return std::max(v, 1e-30);
}

double db10(double ratio) {
    return 10.0 * std::log10(clampPositive(ratio));
}

}  // namespace

SndrResult calSndr(const QVector<double> &data, double fs, double fb, const FftAnalysisConfig &cfg) {
    SndrResult r;
    if (data.size() < 16 || fs <= 0.0 || fb <= 0.0 || fb > fs / 2.0) {
        r.error = QStringLiteral("invalid input");
        return r;
    }

    const int lenFft = data.size();
    const double resolution = fs / static_cast<double>(lenFft);
    const int lenDisplay = lenFft / 2;
    const int dcBins = std::max(0, cfg.dcBins);
    const int signalBins = cfg.signalBins > 0 ? cfg.signalBins : defaultSignalBinsForWindow(cfg.window);
    // ADC_Analyzer_cpp searches past the first five bins independently of the
    // configurable DC integration span.
    constexpr int kReferencePeakSearchStart = 5;
    const int searchStart =
        std::min(kReferencePeakSearchStart, std::max(1, lenDisplay - 2));
    if (lenDisplay <= searchStart + 2) {
        r.error = QStringLiteral("fft size too small");
        return r;
    }

    QVector<double> win = makeWindow(lenFft, cfg.window);
    const double mean = std::accumulate(data.begin(), data.end(), 0.0) / static_cast<double>(data.size());

    QVector<std::complex<double>> td;
    td.reserve(lenFft);
    for (int i = 0; i < lenFft; ++i) {
        td.push_back(std::complex<double>((data[i] - mean) * win[i], 0.0));
    }

    const QVector<std::complex<double>> fd = fft(td);
    QVector<double> fftData;
    fftData.reserve(lenDisplay);
    for (int i = 0; i < lenDisplay; ++i) {
        const double mag2 = std::norm(fd[i]);
        fftData.push_back(mag2);
    }

    double winNorm2 = 0.0;
    for (double w : win) {
        winNorm2 += w * w;
    }
    if (winNorm2 <= 0.0) {
        r.error = QStringLiteral("window norm error");
        return r;
    }

    for (double &v : fftData) {
        v = v / winNorm2 / fs;
    }

    QVector<double> fftFreq;
    fftFreq.reserve(lenDisplay);
    for (int i = 0; i < lenDisplay; ++i) {
        fftFreq.push_back(static_cast<double>(i) * resolution);
    }
    // A spectrum remains meaningful when tone-based metrics cannot be
    // integrated (DC input, missing tone, or an invalid analysis band).
    r.fftData = fftData;
    r.fftFreq = fftFreq;

    const int sigbandBins = std::min(std::max(1, 1 + static_cast<int>(std::llround(fb / resolution))), lenDisplay);
    const int sigbandEndExclusive = sigbandBins - 1;
    if (sigbandEndExclusive <= searchStart) {
        r.error = QStringLiteral("analysis bandwidth is too narrow for peak search");
        return r;
    }
    // Preserve DataEngine.cpp's end-exclusive bin ranges. Its calculation uses
    // a 1-based signal-bin variable even though fftData itself is 0-based.
    const double sigbandPower =
        resolution * sumSlice(fftData, 0, sigbandBins - 1);

    int argMax = searchStart;
    const int searchEnd = std::max(searchStart + 1, sigbandBins);
    for (int i = searchStart; i < searchEnd; ++i) {
        if (fftData[i] > fftData[argMax]) {
            argMax = i;
        }
    }
    const int bin = std::min(std::max(1, argMax), lenDisplay - 1);
    const double fin = static_cast<double>(bin) * resolution;

    const int signalStart = bin + 1 - signalBins;
    const int signalEndExclusive = bin + 1 + signalBins;
    if (signalStart < dcBins) {
        r.error = QStringLiteral("signal integration bins overlap the DC exclusion bins");
        return r;
    }
    if (signalStart < 0 || signalEndExclusive > sigbandEndExclusive) {
        r.error = QStringLiteral("signal integration bins exceed the configured bandwidth");
        return r;
    }

    const double sigPower =
        resolution * sumSlice(fftData, signalStart, signalEndExclusive);
    const double dcPower = resolution * sumSlice(fftData, 0, dcBins);

    const double noiseAndDistortionPower = sigbandPower - sigPower - dcPower;
    const double powerTolerance = qMax(1e-30, std::abs(sigbandPower) * 1e-12);
    if (sigPower <= 0.0) {
        r.error = QStringLiteral("signal power is zero");
        return r;
    }
    if (noiseAndDistortionPower < -powerTolerance) {
        r.error = QStringLiteral("signal/DC integration exceeds in-band power");
        return r;
    }

    auto harmonicPower = [&](int order) -> double {
        const double harmonicFreq = static_cast<double>(order) * fin;
        if (harmonicFreq >= fb) {
            return 0.0;
        }
        const int hBin = static_cast<int>(std::llround(harmonicFreq / resolution));
        return resolution *
               sumSlice(fftData, hBin + 1 - signalBins, hBin + 1 + signalBins);
    };

    const double thdPower2 = harmonicPower(2);
    const double thdPower3 = harmonicPower(3);
    const double thdPower4 = harmonicPower(4);
    const double thdPower5 = harmonicPower(5);

    const double harmonicTotal = thdPower2 + thdPower3 + thdPower4 + thdPower5;

    const double sndrDb = db10(sigPower / clampPositive(noiseAndDistortionPower));
    const double thdDb = 20.0 * std::log10(std::sqrt(clampPositive(sigPower / clampPositive(harmonicTotal))));
    const double enob = (sndrDb - 1.76) / 6.02;

    const double rawNoiseOnlyPower =
        sigbandPower - sigPower - dcPower - harmonicTotal;
    if (rawNoiseOnlyPower < -powerTolerance) {
        r.error = QStringLiteral("harmonic integration overlaps signal or bandwidth bins");
        return r;
    }
    const double noiseOnlyPower = qMax(0.0, rawNoiseOnlyPower);
    // Match the reference ADC_Analyzer_CPP DataEngine: IRN integrates all in-band
    // spectral power except DC. It is not the same as the noise-only SNR term.
    const double irnPower = qMax(0.0, sigbandPower - dcPower);
    const double irnPowerDb = db10(irnPower) + 3.0;
    const double irn = std::sqrt(clampPositive(irnPower));
    const double thdOddDb = db10(sigPower / clampPositive(thdPower3 + thdPower5));
    const double thdEvenDb = db10(sigPower / clampPositive(thdPower2 + thdPower4));
    const double snrDb = db10(sigPower / clampPositive(noiseOnlyPower));
    const double largestHarmonic = std::max(std::max(thdPower2, thdPower3), std::max(thdPower4, thdPower5));
    const double sfdrDb = db10(sigPower / clampPositive(largestHarmonic));
    const double fomDb = sndrDb + db10(fb / 22e-6);

    r.sndrDb = sndrDb;
    r.enob = enob;
    r.irn = irn;
    r.fin = fin;
    r.fftData = fftData;
    r.fftFreq = fftFreq;
    r.irnPowerDb = irnPowerDb;
    r.thdDb = thdDb;
    r.thdOddDb = thdOddDb;
    r.thdEvenDb = thdEvenDb;
    r.snrDb = snrDb;
    r.sfdrDb = sfdrDb;
    r.fomDb = fomDb;
    r.ok = true;
    return r;
}

SndrResult calSndr(const QVector<double> &data, double fs, double fb, const QString &winType) {
    FftAnalysisConfig cfg;
    cfg.window = winType;
    cfg.signalBins = defaultSignalBinsForWindow(winType);
    return calSndr(data, fs, fb, cfg);
}

}  // namespace ccv2
