#include "signal/sndr_calculator.h"

#include <QFile>
#include <QString>
#include <QVector>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1e-30;

struct ReferenceMetrics {
    double sndr{0.0};
    double enob{0.0};
    double irn{0.0};
    double fin{0.0};
    QVector<double> fftData;
    double irnPower{0.0};
    double thd{0.0};
    double thdOdd{0.0};
    double thdEven{0.0};
    double snr{0.0};
    double sfdr{0.0};
    double fom{0.0};
};

double clampPositive(double value) {
    return value > kEps ? value : kEps;
}

double db10(double value) {
    return 10.0 * std::log10(clampPositive(value));
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

double sumRange(const QVector<double> &values, int begin, int endExclusive) {
    const int size = static_cast<int>(values.size());
    const int first = std::clamp(begin, 0, size);
    const int last = std::clamp(endExclusive, 0, size);
    double sum = 0.0;
    for (int i = first; i < last; ++i) {
        sum += values[i];
    }
    return sum;
}

QVector<double> makeReferenceWindow(int n, const QString &name) {
    QVector<double> window(n, 1.0);
    if (name == QStringLiteral("rect")) {
        return window;
    }
    if (name == QStringLiteral("hann")) {
        for (int i = 0; i < n; ++i) {
            window[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (n - 1)));
        }
    } else if (name == QStringLiteral("blackman")) {
        for (int i = 0; i < n; ++i) {
            const double x = 2.0 * kPi * i / (n - 1);
            window[i] = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
        }
    } else if (name == QStringLiteral("blackmanharris")) {
        constexpr double a0 = 3.58750287312166e-1;
        constexpr double a1 = 4.88290107472600e-1;
        constexpr double a2 = 1.41279712970519e-1;
        constexpr double a3 = 1.16798922447150e-2;
        for (int i = 0; i < n; ++i) {
            const double x = 2.0 * kPi * i / static_cast<double>(n);
            window[i] = a0 - a1 * std::cos(x) + a2 * std::cos(2.0 * x) - a3 * std::cos(3.0 * x);
        }
    } else if (name == QStringLiteral("kaiser")) {
        constexpr double beta = 20.0;
        const double denominator = besselI0(beta);
        for (int i = 0; i < n; ++i) {
            const double r = (2.0 * i) / static_cast<double>(n - 1) - 1.0;
            const double argument = beta * std::sqrt(std::max(0.0, 1.0 - r * r));
            window[i] = besselI0(argument) / denominator;
        }
    }
    return window;
}

void fftInPlace(QVector<std::complex<double>> &values) {
    const int n = values.size();
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(values[i], values[j]);
        }
    }

    for (int length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * kPi / length;
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += length) {
            std::complex<double> w(1.0, 0.0);
            for (int offset = 0; offset < length / 2; ++offset) {
                const std::complex<double> even = values[i + offset];
                const std::complex<double> odd = values[i + offset + length / 2] * w;
                values[i + offset] = even + odd;
                values[i + offset + length / 2] = even - odd;
                w *= step;
            }
        }
    }
}

ReferenceMetrics runReference(const QVector<double> &voltage,
                              double fs,
                              double bandwidth,
                              int dcBins,
                              int signalBins,
                              const QString &windowName) {
    ReferenceMetrics out;
    const int n = voltage.size();
    const int displayBins = n / 2;
    const double resolution = fs / static_cast<double>(n);
    const QVector<double> window = makeReferenceWindow(n, windowName);

    double mean = 0.0;
    for (double value : voltage) {
        mean += value;
    }
    mean /= n;

    double windowNormSquared = 0.0;
    QVector<std::complex<double>> spectrum(n);
    for (int i = 0; i < n; ++i) {
        spectrum[i] = std::complex<double>((voltage[i] - mean) * window[i], 0.0);
        windowNormSquared += window[i] * window[i];
    }
    fftInPlace(spectrum);

    out.fftData.resize(displayBins);
    for (int i = 0; i < displayBins; ++i) {
        out.fftData[i] = std::norm(spectrum[i]) / clampPositive(windowNormSquared * fs);
    }

    const int signalBandBins =
        std::clamp(1 + static_cast<int>(std::llround(bandwidth / resolution)), 1, displayBins);
    const double signalBandPower =
        resolution * sumRange(out.fftData, 0, signalBandBins - 1);

    int peakBin = 5;
    double peakPower = -1.0;
    for (int i = 5; i < displayBins; ++i) {
        if (out.fftData[i] > peakPower) {
            peakPower = out.fftData[i];
            peakBin = i;
        }
    }
    peakBin = std::clamp(peakBin, 1, displayBins - 1);
    out.fin = peakBin * resolution;

    // These ranges preserve ADC_Analyzer_cpp/DataEngine.cpp's 1-based-bin
    // translation exactly: [peak + 1 - span, peak + 1 + span).
    const double signalPower =
        resolution * sumRange(out.fftData, peakBin + 1 - signalBins, peakBin + 1 + signalBins);
    const double dcPower = resolution * sumRange(out.fftData, 0, dcBins);
    const double noiseAndDistortionPower = signalBandPower - signalPower - dcPower;

    auto harmonicPower = [&](int order) {
        const double frequency = order * out.fin;
        if (frequency >= bandwidth) {
            return 0.0;
        }
        const int harmonicBin = static_cast<int>(std::llround(frequency / resolution));
        return resolution
               * sumRange(out.fftData,
                          harmonicBin + 1 - signalBins,
                          harmonicBin + 1 + signalBins);
    };

    const double h2 = harmonicPower(2);
    const double h3 = harmonicPower(3);
    const double h4 = harmonicPower(4);
    const double h5 = harmonicPower(5);
    const double harmonicTotal = h2 + h3 + h4 + h5;

    out.sndr = db10(signalPower / clampPositive(noiseAndDistortionPower));
    out.enob = (out.sndr - 1.76) / 6.02;
    const double irnPower = signalBandPower - dcPower;
    out.irnPower = db10(irnPower) + 3.0;
    out.irn = std::sqrt(clampPositive(irnPower));
    out.thd = db10(signalPower / clampPositive(harmonicTotal));
    out.thdOdd = db10(signalPower / clampPositive(h3 + h5));
    out.thdEven = db10(signalPower / clampPositive(h2 + h4));
    out.snr =
        db10(signalPower
             / clampPositive(signalBandPower - signalPower - dcPower - harmonicTotal));
    out.sfdr = db10(signalPower / clampPositive(std::max({h2, h3, h4, h5})));
    out.fom = out.sndr + db10(bandwidth / 22e-6);
    return out;
}

QVector<double> syntheticInput(int n) {
    QVector<double> values;
    values.reserve(n);
    std::uint32_t randomState = 0x13579BDFu;
    for (int i = 0; i < n; ++i) {
        randomState = randomState * 1664525u + 1013904223u;
        const double noise =
            (static_cast<double>((randomState >> 8) & 0xFFFFu) / 65535.0 - 0.5) * 0.0015;
        const double phase = 2.0 * kPi * 181.25 * i / n;
        values.push_back(0.9
                         + 0.52 * std::sin(phase)
                         + 0.018 * std::sin(2.0 * phase + 0.2)
                         + 0.011 * std::sin(3.0 * phase - 0.4)
                         + 0.006 * std::sin(4.0 * phase + 0.7)
                         + noise);
    }
    return values;
}

QVector<double> loadAdcChannel(const QString &path, int channel, int points) {
    constexpr int channels = 256;
    constexpr int bytesPerFrame = channels * static_cast<int>(sizeof(quint32));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QVector<double> values;
    values.reserve(points);
    QByteArray frame(bytesPerFrame, '\0');
    while (values.size() < points && file.read(frame.data(), frame.size()) == frame.size()) {
        const auto *word = reinterpret_cast<const uchar *>(frame.constData() + channel * 4);
        const quint32 raw = qFromLittleEndian<quint32>(word);
        values.push_back(static_cast<double>(raw & 0x0FFFu) / 4096.0 * 1.8);
    }
    return values;
}

bool compareMetric(const char *name, double actual, double reference, double tolerance) {
    const double difference = std::abs(actual - reference);
    std::cout << name << ": v5=" << actual << " reference=" << reference
              << " abs_diff=" << difference << '\n';
    if (difference > tolerance) {
        std::cerr << name << " exceeds tolerance " << tolerance << '\n';
        return false;
    }
    return true;
}

bool compareOne(const char *sourceName,
                const QVector<double> &data,
                double fs,
                double bandwidth,
                const ccv2::FftAnalysisConfig &config) {
    std::cout << "\n[" << sourceName << "] N=" << data.size() << " fs=" << fs
              << " bandwidth=" << bandwidth << " window="
              << config.window.toStdString() << " dcBins=" << config.dcBins
              << " signalBins=" << config.signalBins << '\n';

    const ReferenceMetrics reference =
        runReference(data, fs, bandwidth, config.dcBins, config.signalBins, config.window);
    const ccv2::SndrResult actual = ccv2::calSndr(data, fs, bandwidth, config);
    if (!actual.ok) {
        std::cerr << "V5 calculation failed: " << actual.error.toStdString() << '\n';
        return false;
    }

    bool ok = true;
    ok &= compareMetric("Fin_Hz", actual.fin, reference.fin, 1e-9);
    ok &= compareMetric("SNDR_dB", actual.sndrDb, reference.sndr, 1e-6);
    ok &= compareMetric("ENOB", actual.enob, reference.enob, 1e-7);
    ok &= compareMetric("SNR_dB", actual.snrDb, reference.snr, 1e-6);
    ok &= compareMetric("SFDR_dB", actual.sfdrDb, reference.sfdr, 1e-6);
    ok &= compareMetric("THD_dB", actual.thdDb, reference.thd, 1e-6);
    ok &= compareMetric("THD_odd_dB", actual.thdOddDb, reference.thdOdd, 1e-6);
    ok &= compareMetric("THD_even_dB", actual.thdEvenDb, reference.thdEven, 1e-6);
    ok &= compareMetric("IRN_power_dB", actual.irnPowerDb, reference.irnPower, 1e-6);
    ok &= compareMetric("IRN_Vrms", actual.irn, reference.irn, 1e-9);
    ok &= compareMetric("FOM_dB", actual.fomDb, reference.fom, 1e-6);

    if (actual.fftData.size() != reference.fftData.size()) {
        std::cerr << "FFT length mismatch\n";
        return false;
    }
    double maxSpectrumDifference = 0.0;
    for (int i = 0; i < actual.fftData.size(); ++i) {
        maxSpectrumDifference =
            std::max(maxSpectrumDifference, std::abs(actual.fftData[i] - reference.fftData[i]));
    }
    std::cout << "FFT_power_max_abs_diff=" << maxSpectrumDifference << '\n';
    ok &= maxSpectrumDifference <= 1e-15;
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    constexpr int n = 2048;
    constexpr double fs = 20000.0;
    constexpr double bandwidth = 8000.0;

    ccv2::FftAnalysisConfig config;
    config.dcBins = static_cast<int>(std::llround(100.0 / (fs / n)));

    const QVector<double> synthetic = syntheticInput(n);
    bool ok = true;
    const QStringList windows{
        QStringLiteral("rect"),
        QStringLiteral("hann"),
        QStringLiteral("blackman"),
        QStringLiteral("blackmanharris"),
        QStringLiteral("kaiser"),
    };
    for (const QString &window : windows) {
        config.window = window;
        config.signalBins = window == QStringLiteral("rect")
                                ? 1
                                : (window == QStringLiteral("hann") ? 5 : 7);
        const QByteArray name = QStringLiteral("synthetic/%1").arg(window).toLocal8Bit();
        ok &= compareOne(name.constData(), synthetic, fs, bandwidth, config);
    }

    config.window = QStringLiteral("hann");
    config.dcBins = 7;
    config.signalBins = 3;
    ok &= compareOne("synthetic/custom_bins", synthetic, fs, 6500.0, config);

    if (argc >= 2) {
        const int channel = argc >= 3 ? QString::fromLocal8Bit(argv[2]).toInt() : 239;
        const QVector<double> fileData =
            loadAdcChannel(QString::fromLocal8Bit(argv[1]), channel, n);
        if (fileData.size() != n) {
            std::cerr << "Could not load " << n << " frames from ADC_DATA.bin\n";
            return 2;
        }
        config.window = QStringLiteral("hann");
        config.dcBins = static_cast<int>(std::llround(100.0 / (fs / n)));
        config.signalBins = 5;
        ok &= compareOne("ADC_DATA.bin", fileData, fs, bandwidth, config);
    }
    return ok ? 0 : 1;
}
